// Lockstep online play: every game frame both machines swap their player's controller input and
// the game only advances once it has both. Time is deterministic in this mode (guest_clock.h), so
// the same inputs give the same game on both machines and nothing else needs to be sent. Inputs
// are scheduled `delay` frames ahead, which hides the network round trip as long as it is shorter
// than that delay. See nsmbw_netplay.h for the environment variables.

#include "nsmbw_netplay.h"

#include "guest_clock.h"
#include "guest_flat_memory.h"
#include "nsmbw_savestate.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;
#endif

namespace aurora::gx {
void ForEachCopyTextureRange(void (*visit)(const void* dest, size_t size, void* user), void* user);
}

namespace {

constexpr uint32_t kMagic = 0x4E574E50u;  // "NWNP"
constexpr uint32_t kProtocolVersion = 1;
constexpr uint32_t kHashEveryFrames = 60;
constexpr int kInputsPerPacket = 32;      // redundancy: a lost packet is covered by the next one
constexpr int kTiltScale = 1000;          // tilt travels as an integer, so both sides use the same value

struct InputEntry {
    uint32_t hold = 0;
    int16_t tilt = 0;
};

#pragma pack(push, 1)
struct Packet {
    uint32_t magic;
    uint32_t version;
    uint32_t newestFrame;   // frame of inputs[0]; inputs[i] is newestFrame - i
    uint32_t count;
    uint32_t hashFrame;     // 0xFFFFFFFF when none
    uint64_t hash;
    struct { uint32_t hold; int16_t tilt; } inputs[kInputsPerPacket];
};
#pragma pack(pop)

struct State {
    bool initialized = false;
    bool active = false;
    bool host = false;
    uint32_t delay = 3;
    SocketHandle sock = kNoSocket;
    sockaddr_storage peer{};
    socklen_t peerLen = 0;
    bool havePeer = false;
    std::map<uint32_t, InputEntry> local;
    std::map<uint32_t, InputEntry> remote;
    std::map<uint32_t, uint64_t> localHashes;
    std::map<uint32_t, uint64_t> remoteHashes;
    uint32_t lastHashFrame = 0xFFFFFFFFu;
    uint64_t lastHash = 0;
    uint32_t desyncs = 0;
    FILE* hashFile = nullptr;
};

State g;

// Guest ranges written by the GPU (EFB->texture copies). Their bytes depend on the graphics
// backend (Metal and Direct3D rasterize slightly differently), so the hash skips them.
std::vector<std::pair<uint32_t, uint32_t>> g_gpuRanges;

void CollectGpuRanges()
{
    g_gpuRanges.clear();
    aurora::gx::ForEachCopyTextureRange(
        [](const void* dest, size_t size, void*) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(MKW_FLAT_GUEST_BASE);
            const uintptr_t p = reinterpret_cast<uintptr_t>(dest);
            if (p >= base && p - base < GuestFlat::kGuestSpaceSize && size != 0) {
                g_gpuRanges.emplace_back(static_cast<uint32_t>(p - base), static_cast<uint32_t>(size));
            }
        },
        nullptr);
}

uint64_t HashRange(uint32_t start, uint32_t size, uint64_t h)
{
    constexpr uint32_t kChunk = 0x10000;
    static std::array<uint8_t, kChunk> scratch;
    for (uint32_t off = 0; off < size; off += kChunk) {
        const uint8_t* p = GuestFlat::HostPointer(start + off);
        if (p == nullptr) continue;
        const uint32_t chunkStart = start + off;
        bool masked = false;
        for (const auto& [gs, gz] : g_gpuRanges) {
            const uint64_t lo = std::max<uint64_t>(gs, chunkStart);
            const uint64_t hi = std::min<uint64_t>(uint64_t(gs) + gz, uint64_t(chunkStart) + kChunk);
            if (lo >= hi) continue;
            if (!masked) {
                std::memcpy(scratch.data(), p, kChunk);
                masked = true;
            }
            std::memset(scratch.data() + (lo - chunkStart), 0, static_cast<size_t>(hi - lo));
        }
        if (masked) p = scratch.data();
        const uint64_t* w = reinterpret_cast<const uint64_t*>(p);
        for (uint32_t i = 0; i < kChunk / 8; ++i) {
            h = (h ^ w[i]) * 0x100000001B3ull;
            h ^= h >> 29;
        }
    }
    return h;
}

// MEM1 (game code, globals, most heaps) and MEM2 (the rest of the heaps).
uint64_t HashGuestMemory(std::string* chunkReport)
{
    uint64_t h = 0xCBF29CE484222325ull;
    struct Region { uint32_t start, size; } regions[] = {
        {0x80000000u, 0x01A00000u}, {0x90000000u, 0x08000000u}};  // MEM1 + Newer's overlay, all of MEM2
    for (const auto& r : regions) {
        h = HashRange(r.start, r.size, h);
        if (chunkReport != nullptr) {
            for (uint32_t off = 0; off < r.size; off += 0x100000u) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), " %08X:%08X", r.start + off,
                              static_cast<uint32_t>(HashRange(r.start + off, 0x100000u, 0)));
                *chunkReport += buf;
            }
        }
    }
    return h;
}

void InitOnce()
{
    if (g.initialized) return;
    g.initialized = true;
    if (const char* path = std::getenv("NSMBW_DET_HASH_FILE"); path && *path) {
        g.hashFile = std::fopen(path, "w");
    }
    const char* spec = std::getenv("NSMBW_NETPLAY");
    if (spec == nullptr || *spec == '\0') return;
    if (const char* d = std::getenv("NSMBW_NETPLAY_DELAY"); d && *d) g.delay = static_cast<uint32_t>(std::atoi(d));

    std::string s = spec;
    std::string hostName;
    std::string port;
    if (s.rfind("host:", 0) == 0) {
        g.host = true;
        port = s.substr(5);
    } else if (s.rfind("join:", 0) == 0) {
        const auto colon = s.rfind(':');
        hostName = s.substr(5, colon - 5);
        port = s.substr(colon + 1);
    } else {
        std::fprintf(stderr, "[netplay] NSMBW_NETPLAY must be host:<port> or join:<host>:<port>\n");
        return;
    }

#if defined(_WIN32)
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (g.host) hints.ai_flags = AI_PASSIVE;
    addrinfo* res = nullptr;
    if (getaddrinfo(g.host ? nullptr : hostName.c_str(), port.c_str(), &hints, &res) != 0 || res == nullptr) {
        std::fprintf(stderr, "[netplay] could not resolve %s\n", spec);
        return;
    }
    g.sock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (g.sock == kNoSocket) {
        freeaddrinfo(res);
        std::fprintf(stderr, "[netplay] socket() failed\n");
        return;
    }
    if (g.host) {
        if (::bind(g.sock, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
            std::fprintf(stderr, "[netplay] could not listen on UDP port %s\n", port.c_str());
            freeaddrinfo(res);
            return;
        }
    } else {
        std::memcpy(&g.peer, res->ai_addr, res->ai_addrlen);
        g.peerLen = static_cast<socklen_t>(res->ai_addrlen);
        g.havePeer = true;
    }
    freeaddrinfo(res);
#if defined(_WIN32)
    u_long nonBlocking = 1;
    ioctlsocket(g.sock, FIONBIO, &nonBlocking);
#else
    fcntl(g.sock, F_SETFL, fcntl(g.sock, F_GETFL, 0) | O_NONBLOCK);
#endif
    g.active = true;
    GuestClock::SetDeterministic(true);
    std::fprintf(stderr, "[netplay] %s, player %d, input delay %u frames\n",
                 g.host ? ("hosting on UDP " + port).c_str() : ("joining " + hostName + ":" + port).c_str(),
                 g.host ? 1 : 2, g.delay);
}

void Send(uint32_t newestFrame)
{
    if (!g.havePeer) return;
    Packet p{};
    p.magic = kMagic;
    p.version = kProtocolVersion;
    p.newestFrame = newestFrame;
    p.hashFrame = g.lastHashFrame;
    p.hash = g.lastHash;
    uint32_t n = 0;
    for (; n < kInputsPerPacket && n <= newestFrame; ++n) {
        const auto it = g.local.find(newestFrame - n);
        if (it == g.local.end()) break;
        p.inputs[n].hold = it->second.hold;
        p.inputs[n].tilt = it->second.tilt;
    }
    p.count = n;
    ::sendto(g.sock, reinterpret_cast<const char*>(&p), sizeof(p), 0,
             reinterpret_cast<const sockaddr*>(&g.peer), g.peerLen);
}

void CheckHash(uint32_t frame)
{
    const auto l = g.localHashes.find(frame);
    const auto r = g.remoteHashes.find(frame);
    if (l == g.localHashes.end() || r == g.remoteHashes.end()) return;
    if (l->second != r->second) {
        ++g.desyncs;
        std::fprintf(stderr, "[netplay] DESYNC at frame %u (local %016llX, peer %016llX)\n", frame,
                     static_cast<unsigned long long>(l->second), static_cast<unsigned long long>(r->second));
    } else if (frame % (kHashEveryFrames * 10) == 0) {
        std::fprintf(stderr, "[netplay] in sync at frame %u\n", frame);
    }
    g.localHashes.erase(g.localHashes.begin(), std::next(l));
    g.remoteHashes.erase(g.remoteHashes.begin(), std::next(r));
}

void Receive()
{
    for (;;) {
        Packet p{};
        sockaddr_storage from{};
        socklen_t fromLen = sizeof(from);
        const auto got = ::recvfrom(g.sock, reinterpret_cast<char*>(&p), sizeof(p), 0,
                                    reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (got <= 0) return;
        if (static_cast<size_t>(got) != sizeof(p) || p.magic != kMagic) continue;
        if (p.version != kProtocolVersion) {
            static bool warned = false;
            if (!warned) std::fprintf(stderr, "[netplay] the other machine runs a different netplay version\n");
            warned = true;
            continue;
        }
        if (!g.havePeer) {
            std::memcpy(&g.peer, &from, fromLen);
            g.peerLen = fromLen;
            g.havePeer = true;
            std::fprintf(stderr, "[netplay] player 2 connected\n");
        }
        for (uint32_t i = 0; i < p.count && i < static_cast<uint32_t>(kInputsPerPacket) && i <= p.newestFrame; ++i) {
            g.remote.emplace(p.newestFrame - i, InputEntry{p.inputs[i].hold, p.inputs[i].tilt});
        }
        if (p.hashFrame != 0xFFFFFFFFu) {
            g.remoteHashes[p.hashFrame] = p.hash;
            CheckHash(p.hashFrame);
        }
    }
}

// NSMBW_DET_DUMP=<frame>:<hex address>:<hex size>[:<path>] writes that guest range to a file at
// that frame, to diff two machines byte by byte after a hash mismatch.
void DumpIfRequested(uint32_t frame)
{
    static const char* spec = std::getenv("NSMBW_DET_DUMP");
    if (spec == nullptr || *spec == '\0') return;
    unsigned f = 0, addr = 0, size = 0;
    char path[512] = "det_dump.bin";
    if (std::sscanf(spec, "%u:%x:%x:%511s", &f, &addr, &size, path) < 3 || f != frame) return;
    if (FILE* out = std::fopen(path, "wb")) {
        for (uint32_t off = 0; off < size; off += 0x10000u) {
            const uint8_t* p = GuestFlat::HostPointer(addr + off);
            static const uint8_t zeros[0x10000] = {};
            std::fwrite(p ? p : zeros, 1, std::min<uint32_t>(0x10000u, size - off), out);
        }
        std::fclose(out);
    }
}

// NSMBW_DET_WATCH=<hex address>: log the word there whenever it changes, per frame.
void WatchIfRequested(uint32_t frame)
{
    static const uint32_t addr = [] {
        const char* v = std::getenv("NSMBW_DET_WATCH");
        return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 16)) : 0u;
    }();
    static uint32_t last = 0xDEADBEEFu;
    if (addr == 0) return;
    const uint8_t* p = GuestFlat::HostPointer(addr);
    if (p == nullptr) return;
    const uint32_t v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    if (v != last) {
        float f;
        std::memcpy(&f, &v, 4);
        std::fprintf(stderr, "[det-watch] frame %u %08X = %08X (%g) vnanos %lld\n", frame, addr, v, f,
                     static_cast<long long>(GuestClock::g_virtualNanos.load()));
        last = v;
    }
}

void HashFrame(uint32_t frame)
{
    DumpIfRequested(frame);
    WatchIfRequested(frame);
    if (frame == 0 || frame % kHashEveryFrames != 0) return;
    std::string chunks;
    CollectGpuRanges();
    const uint64_t h = HashGuestMemory(g.hashFile ? &chunks : nullptr);
    if (g.hashFile) {
        std::fprintf(g.hashFile, "frame %u hash %016llX vnanos %lld offthread %llu%s\n", frame,
                     static_cast<unsigned long long>(h), static_cast<long long>(GuestClock::g_virtualNanos.load()),
                     static_cast<unsigned long long>(GuestClock::g_offThreadReads.load()), chunks.c_str());
        std::fflush(g.hashFile);
    }
    if (g.active) {
        g.lastHashFrame = frame;
        g.lastHash = h;
        g.localHashes[frame] = h;
        CheckHash(frame);
    }
}

}  // namespace

bool NsmbwNetplayActive()
{
    InitOnce();
    return g.active;
}

uint32_t NsmbwNetplayPlayerCount() { return 2; }

void NsmbwDeterminismFrame(uint32_t frame)
{
    InitOnce();
    HashFrame(frame);
}

void NsmbwNetplayExchange(uint32_t frame, uint32_t localHold, float localTilt, uint32_t holds[4], float tilts[4])
{
    InitOnce();
    HashFrame(frame);

    const int16_t tiltQ = static_cast<int16_t>(std::lround(std::fmax(-1.0f, std::fmin(1.0f, localTilt)) * kTiltScale));
    g.local[frame + g.delay] = InputEntry{localHold, tiltQ};
    // Frames before the first delayed input carry no input on either side.
    for (uint32_t f = 0; f < g.delay && f <= frame; ++f) {
        g.local.emplace(f, InputEntry{});
        g.remote.emplace(f, InputEntry{});
    }

    using Wall = std::chrono::steady_clock;
    const auto waitStart = Wall::now();
    auto lastSend = Wall::time_point{};
    auto lastNotice = waitStart;
    for (;;) {
        Receive();
        const auto now = Wall::now();
        if (now - lastSend >= std::chrono::milliseconds(4)) {
            Send(frame + g.delay);
            lastSend = now;
        }
        if (g.remote.count(frame) != 0) break;
        if (now - lastNotice >= std::chrono::seconds(2)) {
            std::fprintf(stderr, "[netplay] waiting for %s (frame %u)\n",
                         g.havePeer ? "the other player's input" : "player 2 to connect", frame);
            lastNotice = now;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(250));
    }

    const InputEntry mine = g.local[frame];
    const InputEntry theirs = g.remote[frame];
    for (int c = 0; c < 4; ++c) {
        holds[c] = 0;
        tilts[c] = 0.0f;
    }
    const int myChan = g.host ? 0 : 1;
    const int theirChan = 1 - myChan;
    holds[myChan] = mine.hold;
    tilts[myChan] = static_cast<float>(mine.tilt) / kTiltScale;
    holds[theirChan] = theirs.hold;
    tilts[theirChan] = static_cast<float>(theirs.tilt) / kTiltScale;

    // Keep a little history for resends; drop the rest.
    if (frame > 2u * kInputsPerPacket) {
        g.local.erase(g.local.begin(), g.local.lower_bound(frame - 2u * kInputsPerPacket));
        g.remote.erase(g.remote.begin(), g.remote.lower_bound(frame - 2u * kInputsPerPacket));
    }
}

uint32_t NsmbwInputFrame();

// NSMBW_SAVESTATE_TEST=1: every 4 seconds of play, save, run 30 frames, hash guest memory, rewind,
// run the same 30 frames again and compare. The inputs replay identically (the self-test script's
// state is part of the savestate), so any difference is state the savestate missed.
// Called at the start of every game frame, before that frame's input is read.
void NsmbwRollbackFramePoint(uint32_t frame)
{
    static const bool test = [] {
        const char* v = std::getenv("NSMBW_SAVESTATE_TEST");
        return v && *v && *v != '0';
    }();
    if (!test) return;
    // Not part of the savestate, so it remembers what happened before a rewind.
    static int phase = 0;
    static uint32_t savedFrame = 0;
    static uint64_t firstHash = 0;
    static int passes = 0, fails = 0;
    constexpr uint32_t kSpan = 30;
    if (phase == 1 || phase == 3) {
        // Keep a log per frame of the span, as rollback will (only the first save is rewound to).
        if (frame != savedFrame && NsmbwSavestateSave(1 + static_cast<int>(frame % 8))) return;
    }
    if (phase == 0 && frame >= 240 && frame % 240 == 0) {
        savedFrame = frame;
        phase = 1;
        if (NsmbwSavestateSave(0)) {
            phase = 3;
            std::fprintf(stderr, "[savestate] rewound to frame %u, replaying\n", NsmbwInputFrame());
            return;
        }
        std::fprintf(stderr, "[savestate] saved frame %u in %llu us (%.2f MB stacks+state+last frame's pages)\n", frame,
                     static_cast<unsigned long long>(NsmbwSavestateLastSaveMicros()),
                     NsmbwSavestateSlotBytes(0) / 1048576.0);
        return;
    }
    if ((phase == 1 || phase == 3) && frame == savedFrame + kSpan) {
        CollectGpuRanges();
        const uint64_t h = HashGuestMemory(nullptr);
        if (phase == 1) {
            firstHash = h;
            phase = 2;
            std::fprintf(stderr, "[savestate] frame %u hash %016llX, rewinding to %u\n", frame,
                         static_cast<unsigned long long>(h), savedFrame);
            NsmbwSavestateLoad(0);
        }
        const bool ok = h == firstHash;
        (ok ? passes : fails)++;
        std::fprintf(stderr, "[savestate] replayed to frame %u hash %016llX: %s (%d passed, %d failed)\n", frame,
                     static_cast<unsigned long long>(h), ok ? "MATCH" : "MISMATCH", passes, fails);
        phase = 0;
    }
}

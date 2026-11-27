// Going online from the options menu. See nsmbw_online_lobby.h.
//
// Lobby protocol (UDP, the session's port): the joiner sends HELLO with a fingerprint of the game's
// code; the host answers REJECT if it differs, otherwise WELCOME (the save's size and the settings
// that change how the game runs) and the save in CHUNKs, resent until the joiner says GOTALL. The
// host then sends START a few times and both restart into the session.

#include "nsmbw_online_lobby.h"

#include "guest_flat_memory.h"
#include "nand_path.h"
#include "runtime_config.h"
#include "settings_overlay.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
using LobbySocket = SOCKET;
constexpr LobbySocket kNoLobbySocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
using LobbySocket = int;
constexpr LobbySocket kNoLobbySocket = -1;
#endif

namespace nsmbw_online_lobby {
namespace {

namespace fs = std::filesystem;
namespace settings = settings_overlay::settings;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kMagic = 0x4E574C42u;  // "NWLB"
constexpr size_t kChunk = 1024;
enum MsgType : uint8_t { kHello = 1, kWelcome = 2, kChunkMsg = 3, kGotAll = 4, kStart = 5, kReject = 6 };

#pragma pack(push, 1)
struct Msg {
    uint32_t magic = kMagic;
    uint8_t type = 0;
    uint8_t rollback = 0;
    int8_t aspect = 0;
    int8_t language = 0;
    uint8_t powerUpFreeze = 0;
    uint8_t pad[3]{};
    uint64_t fingerprint = 0;
    uint32_t saveSize = 0;
    uint32_t offset = 0;
    uint16_t length = 0;
    uint8_t data[kChunk]{};
};
#pragma pack(pop)

struct Lobby {
    Phase phase = Phase::Idle;
    std::string status;
    LobbySocket sock = kNoLobbySocket;
    sockaddr_storage peer{};
    socklen_t peerLen = 0;
    bool havePeer = false;
    bool rollback = true;
    std::string hostName;  // joiner: what was typed (without the port)
    uint16_t port = kDefaultPort;
    std::vector<uint8_t> save;
    std::vector<bool> haveChunk;
    bool welcomed = false;
    Msg hostSettings{};
    Clock::time_point lastSend{};
    Clock::time_point startedAt{};
} g;

// The game's code (main.dol's text and Newer's), which both machines must run byte for byte.
uint64_t GameFingerprint()
{
    uint64_t h = 0xCBF29CE484222325ull;
    const auto hashRange = [&](uint32_t start, uint32_t size) {
        for (uint32_t off = 0; off < size; off += 0x10000) {
            const uint8_t* p = GuestFlat::HostPointer(start + off);
            if (!p) continue;
            const uint32_t n = std::min<uint32_t>(0x10000, size - off);
            for (uint32_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
        }
    };
    hashRange(0x80004000u, 0x002EC000u);
    hashRange(0x81800000u, 0x00060000u);
    return h;
}

void CloseSocket()
{
    if (g.sock == kNoLobbySocket) return;
#if defined(_WIN32)
    closesocket(g.sock);
#else
    close(g.sock);
#endif
    g.sock = kNoLobbySocket;
}

bool OpenSocket(bool bindPort)
{
#if defined(_WIN32)
    static bool wsa = [] {
        WSADATA d{};
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)wsa;
#endif
    CloseSocket();
    g.sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (g.sock == kNoLobbySocket) return false;
    if (bindPort) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(g.port);
        if (::bind(g.sock, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
            CloseSocket();
            return false;
        }
    }
#if defined(_WIN32)
    u_long nb = 1;
    ioctlsocket(g.sock, FIONBIO, &nb);
#else
    fcntl(g.sock, F_SETFL, fcntl(g.sock, F_GETFL, 0) | O_NONBLOCK);
#endif
    return true;
}

void SendMsg(const Msg& m)
{
    if (!g.havePeer) return;
    ::sendto(g.sock, reinterpret_cast<const char*>(&m), sizeof(m), 0, reinterpret_cast<const sockaddr*>(&g.peer),
             g.peerLen);
}

void Fail(const std::string& why)
{
    CloseSocket();
    g.phase = Phase::Failed;
    g.status = why;
}

fs::path SessionNandRoot() { return RuntimeConfigFile::ApplicationDataDirectory() / "OnlineNAND"; }

// The save inside a NAND root, wherever its title folder is.
fs::path FindSave(const fs::path& root)
{
    std::error_code ec;
    const fs::path titles = root / "title" / "00010004";
    for (fs::recursive_directory_iterator it(titles, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().filename() == "wiimj2d.sav") return it->path();
    }
    return {};
}

std::vector<uint8_t> ReadOwnSave()
{
    const fs::path save = FindSave(RuntimeNandPath::DiscoverNandRootPath());
    if (save.empty()) return {};
    std::ifstream in(save, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A fresh copy of this machine's NAND with `save` (the host's) as the game's save.
bool PrepareSessionNand(const std::vector<uint8_t>& save)
{
    std::error_code ec;
    const fs::path own = RuntimeNandPath::DiscoverNandRootPath();
    const fs::path session = SessionNandRoot();
    fs::remove_all(session, ec);
    fs::create_directories(session, ec);
    fs::copy(own, session, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    fs::path target = FindSave(session);
    if (target.empty()) {
        // No save on this machine yet: the game's own folder for it (USA rev 2: SMNE).
        target = session / "title" / "00010004" / "534d4e45" / "data" / "wiimj2d.sav";
    }
    if (save.empty()) {
        fs::remove(target, ec);
        return true;
    }
    fs::create_directories(target.parent_path(), ec);
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(save.data()), static_cast<std::streamsize>(save.size()));
    return static_cast<bool>(out);
}

void SetEnv(const char* name, const std::string& value)
{
#if defined(_WIN32)
    SetEnvironmentVariableA(name, value.empty() ? nullptr : value.c_str());
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) unsetenv(name);
    else setenv(name, value.c_str(), 1);
#endif
}

// Starts this game again with the current environment, and ends this process.
[[noreturn]] void Relaunch()
{
    SetEnv("NSMBW_RESET_SAVE", "");
    std::fflush(nullptr);
#if defined(_WIN32)
    std::wstring exe(MAX_PATH, L'\0');
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
    std::wstring cmd = L"\"" + exe + L"\"";
    const std::wstring dir = fs::path(exe).parent_path().wstring();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    ExitProcess(0);
#else
    std::string exe;
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    exe.resize(size);
    _NSGetExecutablePath(exe.data(), &size);
    exe.resize(std::strlen(exe.c_str()));
#else
    exe = fs::read_symlink("/proc/self/exe").string();
#endif
    char* argv[] = {exe.data(), nullptr};
    execv(exe.c_str(), argv);
    std::_Exit(1);
#endif
}

void StartSession(bool host, const std::vector<uint8_t>& save)
{
    g.phase = Phase::Starting;
    if (!PrepareSessionNand(save)) {
        Fail("Couldn't prepare the online save folder");
        return;
    }
    CloseSocket();
    SetEnv("NSMBW_NETPLAY", host ? "host:" + std::to_string(g.port)
                                 : "join:" + g.hostName + ":" + std::to_string(g.port));
    SetEnv("NSMBW_NETPLAY_ROLLBACK", g.rollback ? "1" : "");
    SetEnv("NSMBW_NAND_ROOT", RuntimeConfigFile::PathToUtf8(SessionNandRoot()));
    Relaunch();
}

void Receive()
{
    for (;;) {
        Msg m{};
        sockaddr_storage from{};
        socklen_t fromLen = sizeof(from);
        const auto got = ::recvfrom(g.sock, reinterpret_cast<char*>(&m), sizeof(m), 0,
                                    reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (got <= 0) return;
        if (static_cast<size_t>(got) != sizeof(m) || m.magic != kMagic) continue;

        if (g.phase == Phase::Hosting || g.phase == Phase::SendingSave) {
            if (m.type == kHello) {
                std::memcpy(&g.peer, &from, fromLen);
                g.peerLen = fromLen;
                g.havePeer = true;
                if (m.fingerprint != GameFingerprint()) {
                    Msg r;
                    r.type = kReject;
                    SendMsg(r);
                    g.status = "Player 2 has a different version of the game";
                    continue;
                }
                if (g.phase == Phase::Hosting) {
                    g.phase = Phase::SendingSave;
                    g.status = "Player 2 joined. Sending your save...";
                }
            } else if (m.type == kGotAll && g.phase == Phase::SendingSave) {
                Msg s;
                s.type = kStart;
                for (int i = 0; i < 20; ++i) {
                    SendMsg(s);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                g.status = "Starting...";
                StartSession(true, g.save);
                return;
            }
        } else if (g.phase == Phase::Joining || g.phase == Phase::ReceivingSave) {
            if (m.type == kReject) {
                Fail("The host has a different version of the game");
                return;
            }
            if (m.type == kWelcome && !g.welcomed) {
                g.welcomed = true;
                g.hostSettings = m;
                g.rollback = m.rollback != 0;
                g.save.assign(m.saveSize, 0);
                g.haveChunk.assign((m.saveSize + kChunk - 1) / kChunk, false);
                g.phase = Phase::ReceivingSave;
                g.status = "Connected. Getting the host's save...";
            } else if (m.type == kChunkMsg && g.welcomed) {
                const size_t index = m.offset / kChunk;
                if (index < g.haveChunk.size() && m.offset + m.length <= g.save.size()) {
                    std::memcpy(g.save.data() + m.offset, m.data, m.length);
                    g.haveChunk[index] = true;
                }
            } else if (m.type == kStart && g.welcomed) {
                // Settings that change how the game runs have to match the host's for both games to
                // stay identical; they're saved, so they stay after the session too.
                if (settings::AspectIndex() != g.hostSettings.aspect && g.hostSettings.aspect >= 0) {
                    settings::SetAspectIndex(g.hostSettings.aspect);
                }
                if (settings::LanguageIndex() != g.hostSettings.language && g.hostSettings.language >= 0) {
                    settings::SetLanguageIndex(g.hostSettings.language);
                }
                settings::SetPowerUpFreeze(g.hostSettings.powerUpFreeze != 0);
                g.status = "Starting...";
                StartSession(false, g.save);
                return;
            }
        }
    }
}

}  // namespace

void Host(bool rollback)
{
    g = Lobby{};
    g.rollback = rollback;
    g.port = kDefaultPort;
    if (!OpenSocket(true)) {
        Fail("Couldn't open UDP port " + std::to_string(g.port));
        return;
    }
    g.save = ReadOwnSave();
    g.phase = Phase::Hosting;
    g.status = "Waiting for player 2...";
    g.startedAt = Clock::now();
}

void Join(const std::string& address)
{
    g = Lobby{};
    std::string host = address;
    if (const auto colon = address.rfind(':'); colon != std::string::npos) {
        host = address.substr(0, colon);
        g.port = static_cast<uint16_t>(std::atoi(address.c_str() + colon + 1));
    }
    g.hostName = host;
    if (host.empty() || g.port == 0) {
        Fail("Type the host's address, like 100.97.16.105");
        return;
    }
    if (!OpenSocket(false)) {
        Fail("Couldn't open a network socket");
        return;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(g.port).c_str(), &hints, &res) != 0 || res == nullptr) {
        Fail("Couldn't find " + host);
        return;
    }
    std::memcpy(&g.peer, res->ai_addr, res->ai_addrlen);
    g.peerLen = static_cast<socklen_t>(res->ai_addrlen);
    g.havePeer = true;
    freeaddrinfo(res);
    g.phase = Phase::Joining;
    g.status = "Connecting to " + host + "...";
    g.startedAt = Clock::now();
}

void Cancel()
{
    CloseSocket();
    g = Lobby{};
}

void Poll()
{
    if (g.sock == kNoLobbySocket) return;
    Receive();
    const auto now = Clock::now();
    if (now - g.lastSend < std::chrono::milliseconds(60)) return;
    g.lastSend = now;
    switch (g.phase) {
    case Phase::Joining: {
        Msg m;
        m.type = kHello;
        m.fingerprint = GameFingerprint();
        SendMsg(m);
        if (now - g.startedAt > std::chrono::seconds(15)) {
            Fail("No answer from " + g.hostName + ". Is the host waiting?");
        }
        break;
    }
    case Phase::SendingSave: {
        Msg w;
        w.type = kWelcome;
        w.rollback = g.rollback ? 1 : 0;
        w.aspect = static_cast<int8_t>(settings::AspectIndex());
        w.language = static_cast<int8_t>(settings::LanguageIndex());
        w.powerUpFreeze = settings::PowerUpFreeze() ? 1 : 0;
        w.saveSize = static_cast<uint32_t>(g.save.size());
        SendMsg(w);
        for (size_t off = 0; off < g.save.size(); off += kChunk) {
            Msg c;
            c.type = kChunkMsg;
            c.offset = static_cast<uint32_t>(off);
            c.length = static_cast<uint16_t>(std::min(kChunk, g.save.size() - off));
            std::memcpy(c.data, g.save.data() + off, c.length);
            SendMsg(c);
        }
        break;
    }
    case Phase::ReceivingSave:
        if (std::all_of(g.haveChunk.begin(), g.haveChunk.end(), [](bool b) { return b; })) {
            Msg m;
            m.type = kGotAll;
            SendMsg(m);
            g.status = "Got the save. Waiting for the host to start...";
        }
        break;
    default:
        break;
    }
}

Phase CurrentPhase() { return g.phase; }
std::string Status() { return g.status; }

std::vector<std::string> LocalAddresses()
{
    std::vector<std::string> out;
#if defined(_WIN32)
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, list, &size) == NO_ERROR) {
        for (auto* a = list; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                char text[INET_ADDRSTRLEN]{};
                const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
                inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text));
                out.emplace_back(text);
            }
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* a = list; a; a = a->ifa_next) {
            if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
            char text[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr, text, sizeof(text));
            out.emplace_back(text);
        }
        freeifaddrs(list);
    }
#endif
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](const std::string& s) { return s.rfind("127.", 0) == 0 || s.rfind("169.254.", 0) == 0; }),
              out.end());
    std::stable_sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return (a.rfind("100.", 0) == 0) > (b.rfind("100.", 0) == 0);  // Tailscale first
    });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    if (out.size() > 3) out.resize(3);
    return out;
}

bool InSession()
{
    const char* v = std::getenv("NSMBW_NETPLAY");
    return v != nullptr && *v != '\0';
}

void LeaveSession()
{
    SetEnv("NSMBW_NETPLAY", "");
    SetEnv("NSMBW_NETPLAY_ROLLBACK", "");
    SetEnv("NSMBW_NAND_ROOT", "");
    Relaunch();
}

}  // namespace nsmbw_online_lobby

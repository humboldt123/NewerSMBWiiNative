// In-memory savestates for rollback. A savestate holds:
//   - every mapped guest memory region (MEM1, MEM2, Newer's overlay region ...);
//   - every guest thread's native stack: translated functions are real C++ calls whose locals hold
//     guest registers, so a guest call chain lives on the host stack. Each libco stack is copied
//     from its saved stack pointer (or, for the running one, the current one) up to its top;
//   - host-side runtime state the guest observes later, through the per-module hooks in savestate.h.
// Loading copies all of that back from a separate stack, then longjmps into the Save that wrote
// the slot. The stacks are copied to the same addresses, so pointers into them stay valid.
//
// macOS / Linux (libco fibers) only for now: Windows runs guest threads on Win32 fibers, whose
// saved context is opaque.

#include "nsmbw_savestate.h"

#if defined(_WIN32)
// Not yet on Windows: guest threads there are Win32 fibers, whose saved context is opaque. A
// Windows machine can still play online against a rollback peer, in lockstep.
#include <cstdio>
#include <cstdlib>
bool NsmbwSavestateSave(int) { return false; }
void NsmbwSavestateLoad(int) { std::fprintf(stderr, "[savestate] not supported on Windows\n"); std::abort(); }
bool NsmbwSavestateValid(int) { return false; }
uint32_t NsmbwSavestateFrame(int) { return 0; }
uint64_t NsmbwSavestateLastSaveMicros() { return 0; }
size_t NsmbwSavestateSlotBytes(int) { return 0; }
#else

#include "guest_clock.h"
#include "guest_flat_memory.h"
#include "memory.h"
#include "savestate.h"

#include <array>
#include <chrono>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <vector>

#if !defined(_WIN32)
#include <pthread.h>
#endif

std::vector<void*> GuestFiberStacksForSaveState(void** schedulerFiber);
uint32_t NsmbwInputFrame();

namespace {

constexpr size_t kChunk = 0x10000;
constexpr size_t kFiberStackSize = 16u * 1024u * 1024u;  // host_fiber.h
constexpr size_t kFiberHeaderBytes = 0x100;              // libco's saved registers at the block start
constexpr size_t kSchedulerHandleBytes = 64 * sizeof(unsigned long);  // libco co_active_buffer
constexpr uintptr_t kRedZone = 512;

struct StackCopy {
    uint8_t* lo = nullptr;
    std::vector<uint8_t> bytes;
};

struct HeaderCopy {
    uint8_t* at = nullptr;
    std::vector<uint8_t> bytes;
};

struct Slot {
    bool valid = false;
    uint32_t frame = 0;
    jmp_buf jump;
    std::vector<StackCopy> stacks;
    std::vector<HeaderCopy> headers;
    std::vector<uint8_t> host;
    int64_t virtualNanos = 0;
};

std::array<Slot, kNsmbwSavestateSlots> g_slots;
Slot* g_loading = nullptr;
uint64_t g_lastSaveMicros = 0;
alignas(16) uint8_t g_restoreStack[1 << 20];


uint8_t* MainStackTop()
{
#if defined(__APPLE__)
    return static_cast<uint8_t*>(pthread_get_stackaddr_np(pthread_self()));
#else
    pthread_attr_t attr;
    void* addr = nullptr;
    size_t size = 0;
    pthread_getattr_np(pthread_self(), &attr);
    pthread_attr_getstack(&attr, &addr, &size);
    pthread_attr_destroy(&attr);
    return static_cast<uint8_t*>(addr) + size;
#endif
}

void SaveHost(std::vector<uint8_t>& out)
{
    out.clear();
    SaveState::Writer w{out};
    VI_SaveState(w);
    OsSleep_SaveState(w);
    OsInterrupt_SaveState(w);
    Audio_SaveState(w);
    AX_SaveState(w);
    NandAsync_SaveState(w);
    PpcHelpers_SaveState(w);
    GxHle_SaveState(w);
    Fibers_SaveState(w);
    NsmbwInput_SaveState(w);
}

void LoadHost(const std::vector<uint8_t>& in)
{
    SaveState::Reader r{in.data(), in.data() + in.size()};
    VI_LoadState(r);
    OsSleep_LoadState(r);
    OsInterrupt_LoadState(r);
    Audio_LoadState(r);
    AX_LoadState(r);
    NandAsync_LoadState(r);
    PpcHelpers_LoadState(r);
    GxHle_LoadState(r);
    Fibers_LoadState(r);
    NsmbwInput_LoadState(r);
}

void CopyStack(Slot& s, uint8_t* lo, uint8_t* hi)
{
    StackCopy c;
    c.lo = lo;
    c.bytes.assign(lo, hi);
    s.stacks.push_back(std::move(c));
}

__attribute__((noinline)) void SaveBody(Slot& s)
{
    const auto start = std::chrono::steady_clock::now();
    volatile uint8_t marker = 0;
    const uintptr_t sp = reinterpret_cast<uintptr_t>(&marker) - kRedZone;

    // Guest memory is not copied here: GuestFlat's copy-on-write logs record each page the
    // game changes from now on (CowBeginFrame, called by NsmbwSavestateSave after this).

    // Native stacks: the main (scheduler) stack and every guest thread's libco stack.
    s.stacks.clear();
    s.headers.clear();
    void* scheduler = nullptr;
    const std::vector<void*> fibers = GuestFiberStacksForSaveState(&scheduler);
    bool onFiber = false;
    for (void* handle : fibers) {
        uint8_t* block = static_cast<uint8_t*>(handle);
        uint8_t* top = block + kFiberStackSize;
        const bool current = sp >= reinterpret_cast<uintptr_t>(block) && sp < reinterpret_cast<uintptr_t>(top);
        const uintptr_t savedSp = current ? sp : static_cast<uintptr_t*>(handle)[0] - kRedZone;
        onFiber = onFiber || current;
        CopyStack(s, reinterpret_cast<uint8_t*>(savedSp), top);
        s.headers.push_back(HeaderCopy{block, std::vector<uint8_t>(block, block + kFiberHeaderBytes)});
    }
    {
        uint8_t* top = MainStackTop();
        const uintptr_t savedSp =
            onFiber ? static_cast<uintptr_t*>(scheduler)[0] - kRedZone : sp;
        CopyStack(s, reinterpret_cast<uint8_t*>(savedSp), top);
        uint8_t* h = static_cast<uint8_t*>(scheduler);
        s.headers.push_back(HeaderCopy{h, std::vector<uint8_t>(h, h + kSchedulerHandleBytes)});
    }

    SaveHost(s.host);
    s.virtualNanos = GuestClock::g_virtualNanos.load();
    s.frame = NsmbwInputFrame();
    s.valid = true;
    g_lastSaveMicros = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
}

[[noreturn]] void RestoreBody()
{
    Slot& s = *g_loading;
    GuestFlat::CowRollback(s.frame);
    for (const auto& c : s.stacks) std::memcpy(c.lo, c.bytes.data(), c.bytes.size());
    for (const auto& h : s.headers) std::memcpy(h.at, h.bytes.data(), h.bytes.size());
    LoadHost(s.host);
    GuestClock::g_virtualNanos.store(s.virtualNanos);
    longjmp(s.jump, 1);
}

}  // namespace

bool NsmbwSavestateSave(int slot)
{
    Slot& s = g_slots[static_cast<size_t>(slot)];
    if (setjmp(s.jump) != 0) {
        g_loading = nullptr;
        GuestFlat::CowBeginFrame(s.frame);
        return true;
    }
    SaveBody(s);
    GuestFlat::CowBeginFrame(s.frame);
    return false;
}

void NsmbwSavestateLoad(int slot)
{
    g_loading = &g_slots[static_cast<size_t>(slot)];
    if (!g_loading->valid || !GuestFlat::CowHasFrame(g_loading->frame)) {
        std::fprintf(stderr, "[savestate] cannot rewind to frame %u: its memory log is gone\n", g_loading->frame);
        std::abort();
    }
    uint8_t* top = g_restoreStack + sizeof(g_restoreStack);
    // Run the restore on a separate stack: it overwrites the one this call is on.
#if defined(__aarch64__)
    __asm__ volatile("mov sp, %0\n\tblr %1\n\t" : : "r"(top), "r"(&RestoreBody) : "memory");
#elif defined(__x86_64__)
    __asm__ volatile("mov %0, %%rsp\n\tcall *%1\n\t" : : "r"(top), "r"(&RestoreBody) : "memory");
#endif
    __builtin_unreachable();
}

bool NsmbwSavestateValid(int slot) { return g_slots[static_cast<size_t>(slot)].valid; }
uint32_t NsmbwSavestateFrame(int slot) { return g_slots[static_cast<size_t>(slot)].frame; }
uint64_t NsmbwSavestateLastSaveMicros() { return g_lastSaveMicros; }
size_t NsmbwSavestateSlotBytes(int slot)
{
    const Slot& s = g_slots[static_cast<size_t>(slot)];
    size_t n = s.host.size() + GuestFlat::CowLastFrameBytes();
    for (const auto& c : s.stacks) n += c.bytes.size();
    return n;
}
#endif  // !_WIN32

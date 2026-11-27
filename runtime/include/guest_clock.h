#pragma once
// The clock the guest sees: the time base (mftb / OSGetTime), OSSleep deadlines, VI retraces and
// the audio DMA cadence all read it.
//
// Normally it is the host's steady_clock. In deterministic mode (netplay, or
// NSMBW_DETERMINISTIC=1) it is a virtual clock that only moves at points fixed by the guest's own
// execution, so two machines fed the same inputs see time pass identically:
//   - every guest read of the time base advances it by kReadStep, so busy-wait loops still finish;
//   - when every guest thread is idle, the scheduler advances it by kIdleStep, then waits for the
//     wall clock to catch up, which keeps the game at real-time speed.
// Host-side readers (retrace checks, sleep timers, audio polling) never advance it, so how often
// the host happens to poll changes nothing the guest can observe.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

namespace GuestClock {

using HostClock = std::chrono::steady_clock;
using time_point = HostClock::time_point;
using duration = HostClock::duration;

inline constexpr int64_t kReadStepNanos = 50;
inline constexpr int64_t kIdleStepNanos = 250'000;  // 4 kHz, the audio DMA granularity
// Behind real time by more than this (a host stall), the game slows instead of fast-forwarding.
inline constexpr int64_t kMaxLagNanos = 100'000'000;

inline const time_point g_epoch = HostClock::now();
// Decided before the guest runs its first instruction: boot must already be deterministic.
inline std::atomic<bool> g_deterministic{[] {
    const char* v = std::getenv("NSMBW_DETERMINISTIC");
    const char* net = std::getenv("NSMBW_NETPLAY");
    return (v != nullptr && *v != '\0' && *v != '0') || (net != nullptr && *net != '\0');
}()};
inline std::atomic<int64_t> g_virtualNanos{0};
// The guest runs on the thread that loads the game (the launcher's main thread, which every guest
// fiber shares). Reads from any other host thread (the audio mix worker, the renderer) see the
// clock but never move it: how their reads interleave with the guest's differs between machines.
inline const std::thread::id g_guestThread = std::this_thread::get_id();
inline std::atomic<uint64_t> g_offThreadReads{0};
inline time_point g_wallBase = g_epoch;  // guest thread only

// Set while rollback re-simulates frames: they run as fast as possible (no wall-clock wait) and
// make no sound.
inline std::atomic<bool> g_resimulating{false};

inline bool Deterministic() noexcept { return g_deterministic.load(std::memory_order_relaxed); }
inline void SetDeterministic(bool on) noexcept { g_deterministic.store(on, std::memory_order_relaxed); }

// Guest-visible "now" for host-side checks. Never advances the virtual clock.
inline time_point now() noexcept
{
    if (!Deterministic()) return HostClock::now();
    return g_epoch + std::chrono::nanoseconds(g_virtualNanos.load(std::memory_order_relaxed));
}

// Nanoseconds since start, for the time base. A guest read advances the virtual clock.
inline uint64_t ReadNanosForGuest() noexcept
{
    if (!Deterministic()) {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(HostClock::now() - g_epoch).count());
    }
    if (std::this_thread::get_id() != g_guestThread) {
        g_offThreadReads.fetch_add(1, std::memory_order_relaxed);
        return static_cast<uint64_t>(g_virtualNanos.load(std::memory_order_relaxed));
    }
    return static_cast<uint64_t>(g_virtualNanos.fetch_add(kReadStepNanos, std::memory_order_relaxed));
}

// Called by the scheduler's idle loop in place of its wall-clock wait.
inline void IdleStep() noexcept
{
    const int64_t v = g_virtualNanos.fetch_add(kIdleStepNanos, std::memory_order_relaxed) + kIdleStepNanos;
    const time_point target = g_wallBase + std::chrono::nanoseconds(v);
    if (g_resimulating.load(std::memory_order_relaxed)) return;
    const time_point wall = HostClock::now();
    if (wall < target) {
        std::this_thread::sleep_until(target);
    } else if (wall - target > std::chrono::nanoseconds(kMaxLagNanos)) {
        g_wallBase = wall - std::chrono::nanoseconds(v);
    }
}

}  // namespace GuestClock

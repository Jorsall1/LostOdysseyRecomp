#pragma once
#include <atomic>
#include <cstdint>

// Time the whole process spent suspended (HOME menu, sleep mode): Horizon stops
// every thread, so a timer that measures wall-clock time across it sees a gap.
// The Switch exit watchdog polls every 100 ms; a poll that took far longer was
// suspended (os/switch/switch_runtime.cpp). 2026-11-18: after sleep mode the
// movie watchdog dumped "movie without a frame for 394591 ms" for a movie that
// had been open for 2 s of play.
namespace os::suspend_clock
{
inline std::atomic<uint64_t> g_suspendedNs{0};
inline std::atomic<uint32_t> g_suspensions{0};

// Feeds a periodic loop's wake-up times; a gap over `threshold` counts as suspended, less the expected `period`.
struct Detector
{
    uint64_t last = 0;
    uint64_t period = 100'000'000ull, threshold = 2'000'000'000ull;
    // Returns the suspended time this observation found (0 for a normal wake-up).
    uint64_t Observe(uint64_t nowNs)
    {
        const uint64_t previous = last;
        last = nowNs;
        if (!previous || nowNs <= previous) return 0;
        const uint64_t gap = nowNs - previous;
        return gap > threshold ? gap - period : 0;
    }
};

inline void Add(uint64_t ns)
{
    if (!ns) return;
    g_suspendedNs.fetch_add(ns, std::memory_order_relaxed);
    g_suspensions.fetch_add(1, std::memory_order_relaxed);
}
inline uint64_t SuspendedNs() { return g_suspendedNs.load(std::memory_order_relaxed); }
inline uint32_t Suspensions() { return g_suspensions.load(std::memory_order_relaxed); }
// Wall-clock nanoseconds between two readings, less the suspended time between them.
inline uint64_t AwakeNs(uint64_t wallNs, uint64_t suspendedAtStart)
{
    const uint64_t suspended = SuspendedNs() - suspendedAtStart;
    return wallNs > suspended ? wallNs - suspended : 0;
}
}

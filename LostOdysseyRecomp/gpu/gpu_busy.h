#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

// GPU busy time for the Detailed performance overlay (ui/performance_overlay.cpp).
// While the overlay asks for it, timestamp queries bracket each renderer queue
// batch and the presentation command list; their results are read after the
// fences the renderer and presentation already wait for, never with an extra
// GPU wait. The command processor thread records and reads both.
namespace gpu::gpu_busy
{
// The union of batch intervals. Two GPU slots are in flight at once, and a
// batch's begin timestamp lands while the previous batch is still executing:
// summing end - begin counted the overlap twice (Switch 2, 2026-11-18:
// gpu_pct 158 in a GPU-bound battle, and dynamic resolution saw 1.5x the
// real cost). Only the part of a batch after the latest end seen so far adds.
//
// Results do not arrive in GPU order: the presentation list is read right after
// its own fence, a renderer slot only when that slot is reused, frames later.
// Counting only past the latest end seen dropped every renderer batch that was
// read after a later present (Switch 2, 2026-11-18: gpu_ms 1.5-2.7 while single
// batches took 11-41 ms, so dynamic resolution saw an idle GPU and held 1080
// rows at 28 FPS). The union keeps the recent intervals instead, merged and
// sorted, and adds only the part of a new one no recorded interval covers.
struct BusyUnion
{
    // Intervals ending more than this before the newest end are forgotten; a
    // result read later than that counts in full.
    static constexpr uint64_t kWindowNs = 2'000'000'000;
    static constexpr size_t kMaxIntervals = 256;
    static constexpr uint64_t kMaxSpanNs = 1'000'000'000;
    std::vector<std::pair<uint64_t, uint64_t>> intervals; // disjoint, sorted by begin
    uint64_t newestEnd = 0;
    uint64_t total = 0;
    // Returns the time added. Unwritten (0) or reversed results add nothing.
    uint64_t Add(uint64_t begin, uint64_t end)
    {
        if (!begin || end <= begin) return 0;
        // No batch runs for a second (the GPU would be reset first): a span that
        // long is a stale or torn result, and kept it would cover every later batch.
        if (end - begin > kMaxSpanNs) return 0;
        uint64_t covered = 0, mergedBegin = begin, mergedEnd = end;
        auto first = std::lower_bound(intervals.begin(), intervals.end(), begin,
            [](const auto& interval, uint64_t value) { return interval.second < value; });
        auto last = first;
        for (; last != intervals.end() && last->first <= end; ++last) {
            covered += std::min(end, last->second) - std::max(begin, last->first);
            mergedBegin = std::min(mergedBegin, last->first);
            mergedEnd = std::max(mergedEnd, last->second);
        }
        first = intervals.erase(first, last);
        intervals.insert(first, {mergedBegin, mergedEnd});
        newestEnd = std::max(newestEnd, end);
        if (newestEnd > kWindowNs) {
            const uint64_t horizon = newestEnd - kWindowNs;
            auto keep = intervals.begin();
            while (keep != intervals.end() && keep->second < horizon) ++keep;
            intervals.erase(intervals.begin(), keep);
        }
        if (intervals.size() > kMaxIntervals)
            intervals.erase(intervals.begin(), intervals.begin() + (intervals.size() - kMaxIntervals));
        const uint64_t added = (end - begin) - std::min(covered, end - begin);
        total += added;
        return added;
    }
};

inline std::atomic<bool> g_requested{false};
// Dynamic resolution (gpu/dynamic_resolution.h) needs the measurement for as long as it runs, whatever the overlay does.
inline std::atomic<bool> g_dynamicResolution{false};
// So does the perf line (debug/perf_stats.cpp, LO_PERF_GPU), whatever the overlay does.
inline std::atomic<bool> g_perf{false};
inline std::atomic<uint64_t> g_busy{0};
// The longest single batch (end - begin, overlap included) since the last TakeMaxBatchNs: the perf line's
// gpu_batch_max_ms. A very long submission is the first suspect for a GPU device loss.
inline std::atomic<uint64_t> g_maxBatch{0};
inline std::mutex g_unionMutex;
inline BusyUnion g_union;

inline bool Requested()
{
    return g_requested.load(std::memory_order_relaxed) || g_dynamicResolution.load(std::memory_order_relaxed) ||
        g_perf.load(std::memory_order_relaxed);
}
inline void Request(bool requested) { g_requested.store(requested, std::memory_order_relaxed); }
// One completed batch's begin and end timestamps, as plume returns them
// (nanoseconds on Vulkan and D3D12). Unwritten or reordered results add nothing.
inline void AddBatch(uint64_t begin, uint64_t end)
{
    if (!begin || end <= begin) return;
    uint64_t added;
    {
        std::lock_guard lock(g_unionMutex);
        added = g_union.Add(begin, end);
    }
    if (added) g_busy.fetch_add(added, std::memory_order_relaxed);
    uint64_t span = end - begin, seen = g_maxBatch.load(std::memory_order_relaxed);
    while (span > seen && !g_maxBatch.compare_exchange_weak(seen, span, std::memory_order_relaxed)) {}
}
// Running total of AddBatch (overlap counted once).
inline uint64_t Total() { return g_busy.load(std::memory_order_relaxed); }
inline uint64_t TakeMaxBatchNs() { return g_maxBatch.exchange(0, std::memory_order_relaxed); }
inline uint64_t PeekMaxBatchNs() { return g_maxBatch.load(std::memory_order_relaxed); }
}

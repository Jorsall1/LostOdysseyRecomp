#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>

// What the GPU ran just before it stopped (Switch 2 device losses, 2026-11-18:
// raw_vk=-4 0.6 s into the first field map, docked at 1440p, and earlier at
// 1080p in battle). The renderer notes each queue batch it submits and, once
// its fence completes, the batch's GPU time; video::StopGpuWork's caller
// prints the last kEntries as error lines.
namespace gpu::submission_ring
{
struct Entry
{
    uint64_t serial = 0;       // renderer batch number
    uint64_t frame = 0;        // renderer frame
    uint64_t submitMs = 0;     // steady clock at submit
    uint32_t draws = 0, resolves = 0, rectLists = 0, occlusionQueries = 0;
    uint32_t width = 0, height = 0; // internal scene size
    uint32_t scene = 0;        // renderer scene tag
    uint32_t gpuUs = 0;        // completed batch GPU time (end - begin), 0 until completed or without timestamps
    bool completed = false;
};

template <size_t N = 16>
struct Ring
{
    static constexpr size_t kEntries = N;
    std::array<Entry, N> entries{};
    uint64_t next = 0; // entries written

    Entry& Push(const Entry& entry)
    {
        Entry& slot = entries[next % N];
        slot = entry;
        ++next;
        return slot;
    }
    // The batch `serial` completed after `gpuUs` of GPU time.
    void Complete(uint64_t serial, uint32_t gpuUs)
    {
        for (auto& e : entries)
            if (e.serial == serial && serial) { e.completed = true; e.gpuUs = gpuUs; return; }
    }
    size_t Size() const { return next < N ? size_t(next) : N; }
    // Oldest first: i = 0 is the oldest kept entry.
    const Entry& At(size_t i) const { return entries[(next - Size() + i) % N]; }
};

// Switch 2: a renderer batch ends after this many draws as well as when its descriptor pools fill, so no single
// submission runs for very long (a per-submission GPU timeout is the leading suspect for the device losses).
// LO_SWITCH_BATCH_DRAWS=<n> (0: no draw limit, the old behaviour; otherwise at least 64).
#ifdef __SWITCH2__
inline constexpr uint32_t kDefaultBatchDraws = 1024;
#else
inline constexpr uint32_t kDefaultBatchDraws = 0;
#endif
inline uint32_t BatchDrawLimit(const char* value)
{
    if (!value || !*value) return kDefaultBatchDraws;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end) return kDefaultBatchDraws;
    if (parsed == 0) return 0;
    return parsed < 64 ? 64u : parsed > 1u << 20 ? 1u << 20 : uint32_t(parsed);
}
// Whether the open batch, with `batchDraws` draws recorded, ends before the next draw.
inline constexpr bool SplitBeforeDraw(uint32_t batchDraws, uint32_t limit) { return limit && batchDraws >= limit; }
}

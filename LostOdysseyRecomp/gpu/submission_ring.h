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
    uint32_t textureUploads = 0; // guest textures created (and copied in) in this batch
    uint64_t uploadBytes = 0;  // bytes written to the upload ring in this batch
    bool completed = false;
    bool rejected = false;     // pushed before the queue call and cleared by Accept: still set, the queue refused it
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
    // The queue took batch `serial`.
    void Accept(uint64_t serial)
    {
        for (auto& e : entries)
            if (e.serial == serial && serial) { e.rejected = false; return; }
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
// Switch 2 (turn 36): the device losses at the first field map (u11_0_scrw) are not GPU faults. OunceVK's
// native_ctx_flush_locked fails in native_ctx_prepare_batch (the "context failed at nvkmd_nvgpu_native_ctx.c:738"
// line), which checks the batch's command ranges against fixed per-submission limits (count, total size, the
// primary arena) before anything reaches the GPU. The batch it refused is the first frame of a new map, when many
// textures are created and copied in at once. A batch therefore also ends after this many texture uploads or upload
// bytes, so a map's first frame becomes several ordinary submissions. LO_SWITCH_BATCH_UPLOADS=<n> (0: no limit).
#ifdef __SWITCH2__
inline constexpr uint32_t kDefaultBatchUploads = 96;
inline constexpr uint64_t kBatchUploadBytes = 24ull << 20;
#else
inline constexpr uint32_t kDefaultBatchUploads = 0;
inline constexpr uint64_t kBatchUploadBytes = 0;
#endif
inline uint32_t BatchUploadLimit(const char* value)
{
    if (!value || !*value) return kDefaultBatchUploads;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end) return kDefaultBatchUploads;
    if (parsed == 0) return 0;
    return parsed < 8 ? 8u : parsed > 1u << 16 ? 1u << 16 : uint32_t(parsed);
}
// Whether the open batch, with `uploads` texture uploads and `bytes` upload bytes, ends before the next draw.
inline constexpr bool SplitForUploads(uint32_t uploads, uint64_t bytes, uint32_t limit)
{
    return limit && (uploads >= limit || (kBatchUploadBytes && bytes >= kBatchUploadBytes));
}
}

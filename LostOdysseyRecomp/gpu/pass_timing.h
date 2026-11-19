#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>

// Where a renderer batch's GPU time goes (LO_GPU_PASS_TIMING=1, off by default; nothing is recorded when off).
// A batch writes a timestamp each time the kind of work changes, and the time between two timestamps goes to the
// kind that started at the first. Timestamps are bottom-of-pipe, so a boundary is approximate by the draws in
// flight: good for shares of the frame, not for single draws.
namespace gpu::pass_timing
{
enum class Category : uint8_t { Scene, Shadow, RectList, Resolve, Count };
inline constexpr const char* Name(Category c)
{
    switch (c) {
    case Category::Scene: return "scene";
    case Category::Shadow: return "shadow";
    case Category::RectList: return "gs_rect";
    case Category::Resolve: return "resolve";
    default: return "?";
    }
}
inline constexpr size_t kCategories = size_t(Category::Count);
inline constexpr uint32_t kQueries = 512; // per batch; work past it stays in the last category

inline bool Enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("LO_GPU_PASS_TIMING");
        return value && value[0] == '1';
    }();
    return enabled;
}

// One batch's timeline as recorded: timestamp i starts a segment of categories[i]; the last timestamp ends it.
struct Timeline
{
    std::array<Category, kQueries> categories{};
    uint32_t used = 0; // timestamps written

    // Before the first draw. Returns the query index to write, or ~0u.
    uint32_t Begin(Category start = Category::Scene)
    {
        used = 1;
        categories[0] = start;
        return 0;
    }
    // A draw or pass of `c`: the query index to write when it starts a new segment, else ~0u. One query is always
    // kept for End.
    uint32_t Mark(Category c)
    {
        if (!used || categories[used - 1] == c || used + 1 >= kQueries) return ~0u;
        categories[used] = c;
        return used++;
    }
    // At the batch's end: the index of the closing timestamp.
    uint32_t End()
    {
        if (!used) return ~0u;
        return used++;
    }
};

// Adds the batch's segments to `totals` (nanoseconds). Unwritten (0) or reversed timestamps add nothing.
inline void Collect(const Timeline& timeline, const uint64_t* timestamps, std::array<uint64_t, kCategories>& totals)
{
    if (timeline.used < 2 || !timestamps) return;
    for (uint32_t i = 0; i + 1 < timeline.used; ++i) {
        const uint64_t begin = timestamps[i], end = timestamps[i + 1];
        if (begin && end > begin) totals[size_t(timeline.categories[i])] += end - begin;
    }
}
}

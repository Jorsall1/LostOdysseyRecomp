// gpu/pass_timing.h: per-category GPU time from a batch's timestamps.
#include <gpu/pass_timing.h>
#include <cstdio>
#include <cstdlib>
#include <string_view>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main()
{
    using namespace gpu::pass_timing;
    {
        Timeline t;
        CHECK(t.Mark(Category::Shadow) == ~0u); // not begun
        CHECK(t.Begin() == 0);
        CHECK(t.Mark(Category::Scene) == ~0u);  // same category: no timestamp
        CHECK(t.Mark(Category::Shadow) == 1);
        CHECK(t.Mark(Category::Shadow) == ~0u);
        CHECK(t.Mark(Category::Resolve) == 2);
        CHECK(t.Mark(Category::Scene) == 3);
        CHECK(t.End() == 4);
        // scene 0-100, shadow 100-400, resolve 400-450, scene 450-1000
        const uint64_t ts[] = {1000, 1100, 1400, 1450, 2000};
        std::array<uint64_t, kCategories> totals{};
        Collect(t, ts, totals);
        CHECK(totals[size_t(Category::Scene)] == 100 + 550);
        CHECK(totals[size_t(Category::Shadow)] == 300);
        CHECK(totals[size_t(Category::Resolve)] == 50);
        CHECK(totals[size_t(Category::RectList)] == 0);
        // An unwritten timestamp drops its two segments, nothing else.
        const uint64_t partial[] = {1000, 0, 1400, 1450, 2000};
        std::array<uint64_t, kCategories> some{};
        Collect(t, partial, some);
        CHECK(some[size_t(Category::Scene)] == 550 && some[size_t(Category::Shadow)] == 0 && some[size_t(Category::Resolve)] == 50);
    }
    {
        // Capacity: one query always stays for End.
        Timeline t;
        t.Begin();
        uint32_t written = 1;
        for (uint32_t i = 0; i < 2 * kQueries; ++i)
            if (t.Mark(i % 2 ? Category::Scene : Category::Shadow) != ~0u) ++written;
        const uint32_t end = t.End();
        CHECK(end == kQueries - 1 && written == kQueries - 1);
    }
    CHECK(std::string_view(Name(Category::RectList)) == "gs_rect");
    if (failures) return EXIT_FAILURE;
    std::puts("pass_timing_test: ok");
    return 0;
}

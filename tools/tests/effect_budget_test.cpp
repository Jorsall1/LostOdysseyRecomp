// gpu/effect_budget.h (turn 39). Build: g++ -std=c++20 -I LostOdysseyRecomp tools/tests/effect_budget_test.cpp
#include <gpu/effect_budget.h>
#include <cstdio>
namespace eb = gpu::effect_budget;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
// Runs at 60 FPS for `seconds` with `gpuMs` per frame.
static void Run(eb::Controller& c, uint64_t& now, uint64_t& busy, double seconds, double gpuMs, bool* sawUnsafe = nullptr)
{
    for (double t = 0; t < seconds; t += 1.0 / 60) {
        c.Update(now, busy, 14.0);
        // The blur may only be skipped while the weight is zero.
        if (sawUnsafe && c.DofBlurSkip() && !c.DofWeightZero()) *sawUnsafe = true;
        busy += uint64_t(gpuMs * 1e6);
        now += 16'666'667ull;
    }
}
int main()
{
    {
        eb::Controller c;
        uint64_t now = 1'000'000'000ull, busy = 0;
        bool unsafe = false;
        Run(c, now, busy, 5, 12.0, &unsafe);
        CHECK(c.level == 0 && !c.DofWeightZero() && !c.DofBlurSkip());
        // Over budget: level 1 within ~1.5 s, weight zero first, blur skipped 30 frames later.
        Run(c, now, busy, 1.6, 16.0, &unsafe);
        CHECK(c.level >= 1);
        CHECK(c.MotionBlurOff() && c.DofWeightZero());
        Run(c, now, busy, 0.6, 16.0, &unsafe);
        CHECK(c.DofBlurSkip());
        // Still over: level 2 (shadows low) but never past it.
        Run(c, now, busy, 5, 16.0, &unsafe);
        CHECK(c.level == 2 && c.ShadowsLow());
        // In the band between 70% and 100%: holds.
        Run(c, now, busy, 20, 12.0, &unsafe);
        CHECK(c.level == 2);
        // Light scene: steps back down, one level per >= 5 s.
        Run(c, now, busy, 5.5, 8.0, &unsafe);
        CHECK(c.level == 1);
        Run(c, now, busy, 5.5, 8.0, &unsafe);
        CHECK(c.level == 0 && !c.MotionBlurOff());
        // Blur restarts at once, weight returns DofLagFrames later.
        CHECK(!c.DofBlurSkip());
        Run(c, now, busy, 1.0, 8.0, &unsafe);
        CHECK(!c.DofWeightZero());
        CHECK(!unsafe);
    }
    {
        // Loading screens / movies under 3 ms never restore, and a hold keeps the level.
        eb::Controller c;
        uint64_t now = 1'000'000'000ull, busy = 0;
        Run(c, now, busy, 3, 20.0);
        CHECK(c.level >= 1);
        const uint32_t level = c.level;
        Run(c, now, busy, 20, 0.5);
        CHECK(c.level == level);
        for (int i = 0; i < 600; ++i) { c.Hold(now, busy); busy += 30'000'000ull; now += 16'666'667ull; }
        CHECK(c.level == level);
    }
    CHECK(eb::BudgetMs(1000.0 / 60) > 13.9 && eb::BudgetMs(1000.0 / 60) < 14.1);
    if (failures) return 1;
    std::puts("effect_budget_test: ok");
    return 0;
}

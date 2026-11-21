// gpu/effect_budget.h (turn 39). Build: g++ -std=c++20 -I LostOdysseyRecomp tools/tests/effect_budget_test.cpp
#include <gpu/effect_budget.h>
#include <cstdio>
namespace eb = gpu::effect_budget;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
// Runs at 60 FPS for `seconds` with `gpuMs` per frame.
static void Run(eb::Controller& c, uint64_t& now, uint64_t& busy, double seconds, double gpuMs, bool* sawUnsafe = nullptr,
                bool atFloor = true, bool slack = false)
{
    for (double t = 0; t < seconds; t += 1.0 / 60) {
        c.Update(now, busy, 14.0, atFloor, slack);
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
        // Light scene: steps back down; shadows (level 2) only 30 s after their cut, then one level per >= 5 s.
        Run(c, now, busy, 5.5, 8.0, &unsafe);
        Run(c, now, busy, 4.0, 8.0, &unsafe);
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
    {
        // Turn 54: no cut while dynamic resolution still has rows to drop.
        eb::Controller c;
        uint64_t now = 1'000'000'000ull, busy = 0;
        Run(c, now, busy, 5, 35.0, nullptr, false);
        CHECK(c.level == 0);
        // At its floor: cuts.
        Run(c, now, busy, 1.6, 20.0, nullptr, true);
        CHECK(c.level == 1);
        // 13 ms (over 70%) with two rungs of slack: restored after the 5 s hold.
        Run(c, now, busy, 6, 13.0, nullptr, false, true);
        CHECK(c.level == 0);
        // Cut again within 30 s: the next restore waits 10 s.
        Run(c, now, busy, 1.6, 20.0, nullptr, true);
        CHECK(c.level == 1 && c.restoreHoldNs == 10'000'000'000ull);
        Run(c, now, busy, 6, 13.0, nullptr, false, true);
        CHECK(c.level == 1);
        Run(c, now, busy, 5, 13.0, nullptr, false, true);
        CHECK(c.level == 0);
        // Without slack 13 ms holds the level.
        eb::Controller d;
        Run(d, now, busy, 3, 20.0);
        CHECK(d.level >= 1);
        const uint32_t level = d.level;
        Run(d, now, busy, 20, 13.0);
        CHECK(d.level == level);
    }
    {
        // Turn 72: shadows low are not restored through the slack (13 ms), only under 70% and 30 s after the cut.
        eb::Controller c;
        uint64_t now = 1'000'000'000ull, busy = 0;
        Run(c, now, busy, 4, 20.0);
        CHECK(c.level == 2);
        Run(c, now, busy, 60, 13.0, nullptr, false, true);
        CHECK(c.level == 2);
        eb::Controller d;
        Run(d, now, busy, 4, 20.0);
        CHECK(d.level == 2);
        Run(d, now, busy, 20, 8.0);
        CHECK(d.level == 2);                 // under 70%, but only 20 s after the cut
        Run(d, now, busy, 11, 8.0);
        CHECK(d.level == 1);
        // Level 1 still comes back through the slack after its 5 s.
        Run(d, now, busy, 6, 13.0, nullptr, false, true);
        CHECK(d.level == 0);
    }
    CHECK(eb::BudgetMs(1000.0 / 60) > 13.9 && eb::BudgetMs(1000.0 / 60) < 14.1);
    if (failures) return 1;
    std::puts("effect_budget_test: ok");
    return 0;
}

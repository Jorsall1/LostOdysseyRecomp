// gpu/dynamic_resolution.h on the Switch 2 range (turn 35): 1080 -> 576 handheld, 1440 -> 1152 docked.
// Build with -D__SWITCH2__ for the Switch 2 Highest (1440).
#include <gpu/dynamic_resolution.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace dr = gpu::dynamic_resolution;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

// Runs the controller at 60 FPS for `seconds`, with the GPU's ms per frame from cost(height).
template <typename Cost>
static uint32_t Run(dr::Controller& c, uint64_t& now, uint64_t& busy, double seconds, uint32_t base, uint32_t minimum,
                    Cost cost, uint32_t* lowest = nullptr)
{
    const uint64_t frameNs = 16'666'667ull;
    uint32_t height = c.height ? c.height : base;
    for (double t = 0; t < seconds; t += 1.0 / 60) {
        bool changed = false;
        height = c.Update(now, busy, base, minimum, 1000.0 / 60, changed);
        if (lowest && height < *lowest) *lowest = height;
        busy += uint64_t(cost(height) * 1e6);
        now += frameNs;
    }
    return height;
}

int main()
{
#ifdef __SWITCH2__
    CHECK(dr::Highest == 1440);
    CHECK(dr::Supports(1080) && dr::Supports(1440));
    CHECK(dr::Floor(1080, 576) == 576);
    CHECK(dr::Floor(1440, 1152) == 1152);
    CHECK(dr::IsDynamicHeight(1404) && dr::IsDynamicHeight(1152) && dr::IsDynamicHeight(576));
#endif
    // GPU cost follows the pixel count: 48 ms at 1080 rows (the turn-34 battle).
    const auto battle = [](uint32_t h) { return 48.0 * double(h) * h / (1080.0 * 1080.0); };
    {
        dr::Controller c;
        uint64_t now = 1'000'000'000ull, busy = 1;
        uint32_t lowest = 1080;
        const uint32_t h = Run(c, now, busy, 10, 1080, 576, battle, &lowest);
        CHECK(h == 576);       // one jump to the floor: 48 ms needs well below 576 to fit
        CHECK(lowest >= 576);  // never below the floor
    }
    {
        // Moderate load: 22 ms at 1080 fits at ~836 rows (80% budget), stays above the floor.
        const auto mid = [](uint32_t h) { return 22.0 * double(h) * h / (1080.0 * 1080.0); };
        dr::Controller c;
        uint64_t now = 1'000'000'000ull, busy = 1;
        const uint32_t h = Run(c, now, busy, 6, 1080, 576, mid);
        CHECK(h < 1080 && h > 576 && h % 36 == 0);
        CHECK(mid(h) < 1000.0 / 60);
    }
    {
        // Turn 36's measured model: 8 ms fixed + the rest by pixels, 20 ms at 1080 rows. The jump lands
        // where the frame fits, not far below it (pixel-only scaling went much lower).
        const auto field = [](uint32_t h) { return 8.0 + 12.0 * double(h) * h / (1080.0 * 1080.0); };
        dr::Controller c;
        uint64_t now = 1'000'000'000ull, busy = 1;
        const uint32_t h = Run(c, now, busy, 12, 1080, 576, field);
        CHECK(h >= 720 && h < 1080 && h % 36 == 0);
        CHECK(field(h) < 1000.0 / 60);
    }
#ifdef __SWITCH2__
    {
        // Docked: 1440 base, floor 1152 even when the GPU cannot make it.
        const auto docked = [](uint32_t h) { return 70.0 * double(h) * h / (1440.0 * 1440.0); };
        dr::Controller c;
        uint64_t now = 1'000'000'000ull, busy = 1;
        uint32_t lowest = 1440;
        const uint32_t h = Run(c, now, busy, 10, 1440, 1152, docked, &lowest);
        CHECK(h == 1152 && lowest >= 1152);
        // Light scene afterwards: back up to native 1440.
        const uint32_t up = Run(c, now, busy, 60, 1440, 1152, [](uint32_t) { return 6.0; });
        CHECK(up == 1440);
        // Undocked: the base changes and it starts over at 75% of 1080 (turn 54), on a rung of the base.
        bool changed = false;
        CHECK(c.Update(now, busy, 1080, 576, 1000.0 / 60, changed) == 792);
        CHECK(c.minimum == 576);
    }
    {
        // Turn 54: a new docked base starts at 1080 (the title at 19 ms in 1440 rows: 60 FPS at 1080).
        dr::Controller c;
        bool changed = false;
        CHECK(c.Update(1, 1, 1440, 720, 1000.0 / 60, changed) == 1080);
        // A 35 ms entry spike at 1440 that follows the pixels lands well above the 720 floor.
        dr::Controller d;
        uint64_t now = 1'000'000'000ull, busy = 1;
        const auto spike = [](uint32_t h) { return 35.0 * double(h) * h / (1440.0 * 1440.0); };
        d.Update(now, busy, 1440, 720, 1000.0 / 60, changed);
        d.height = 1440;
        uint32_t lowest = 1440;
        const uint32_t h = Run(d, now, busy, 3, 1440, 720, spike, &lowest);
        CHECK(h > 720 && h < 1440 && lowest == h);
        // The field after it (Switch 2 log: 10 ms at 720, 14.2 ms at 972; 5 ms fixed + pixels): several rungs per
        // step up: 936 within 8 s (one rung at a time: 792), and it fits.
        const auto field = [](uint32_t r) { return 5.0 + 5.0 * double(r) * r / (720.0 * 720.0); };
        d.height = 720;
        const uint32_t up = Run(d, now, busy, 8, 1440, 720, field);
        std::printf("field: 720 -> %u rows in 8 s (%.1f ms)\n", up, field(up));
        CHECK(up >= 936 && field(up) < 1000.0 / 60 * 0.92);
    }
#endif
    {
        // Light load at the base: stays native.
        dr::Controller c;
        uint64_t now = 1'000'000'000ull, busy = 1;
        CHECK(Run(c, now, busy, 10, 1080, 576, [](uint32_t) { return 8.0; }) == 1080);
    }
    {
        // Turn 36: a loading screen (0.3 ms) never steps up, and Hold keeps the height and its strikes clear.
        dr::Controller c;
        uint64_t now = 1, busy = 0;
        const auto heavy = [](uint32_t h) { return 30.0 * double(h) * h / (1080.0 * 1080.0); };
        const uint32_t low = Run(c, now, busy, 5, 1080, 576, heavy);
        CHECK(low < 1080);
        const uint32_t after = Run(c, now, busy, 20, 1080, 576, [](uint32_t) { return 0.3; });
        CHECK(after == low);
        c.Hold(now, busy);
        CHECK(c.height == low && c.upStrikes == 0 && c.downStrikes == 0);
        // A light real scene (6 ms at this height) still steps up.
        const uint32_t up = Run(c, now, busy, 20, 1080, 576, [low](uint32_t h) { return 6.0 * double(h) * h / (double(low) * low); });
        CHECK(up > low);
    }
    {
        // Turn 54: 14.2 ms average (under the 15.3 ms line) with 3% of frames missing a refresh steps down one rung,
        // and the height it left is not tried again for 20 s.
        dr::Controller c;
        uint64_t now = 1'000'000'000ull, busy = 1, late = 0;
        bool changed = false;
        c.Update(now, busy, 1080, 576, 1000.0 / 60, changed);
        c.height = 648;
        uint32_t h = 648;
        for (int f = 0; f < 96; ++f) { // 1.6 s: one step (a second needs another second of misses)
            h = c.Update(now, busy, 1080, 576, 1000.0 / 60, changed, dr::Rung, late);
            busy += 14'200'000ull;
            if (f % 30 == 0) ++late; // 1 in 30 frames
            now += 16'666'667ull;
        }
        CHECK(h == 612 && c.ceiling == 648);
        // Light again (11 ms, no misses): may climb back, never to 648 within 20 s.
        uint32_t highest = h;
        for (int f = 0; f < 60 * 15; ++f) {
            h = c.Update(now, busy, 1080, 576, 1000.0 / 60, changed, dr::Rung, late);
            highest = std::max(highest, h);
            busy += 11'000'000ull;
            now += 16'666'667ull;
        }
        CHECK(highest < 648);
        for (int f = 0; f < 60 * 15; ++f) {
            h = c.Update(now, busy, 1080, 576, 1000.0 / 60, changed, dr::Rung, late);
            busy += 11'000'000ull;
            now += 16'666'667ull;
        }
        CHECK(h > 648 && c.ceiling == 0);
        // Misses block a step up even at a light average.
        dr::Controller d;
        now = 1'000'000'000ull; busy = 1; late = 0;
        d.Update(now, busy, 1080, 576, 1000.0 / 60, changed);
        d.height = 612;
        for (int f = 0; f < 60 * 10; ++f) {
            h = d.Update(now, busy, 1080, 576, 1000.0 / 60, changed, dr::Rung, late);
            busy += 9'000'000ull;
            if (f % 50 == 0) ++late; // 2%, average GPU far below the 75% line: no step down either
            now += 16'666'667ull;
        }
        CHECK(h == 612);
    }
    {
        // Turn 72 (Switch 2 handheld, 2026-11-20): 18.0 ms frames at 864 rows with the GPU busy 13.5 ms, no frame
        // late by a refresh. Model: the frame takes the GPU time plus 4 ms (17.5 ms at 864), never less than a refresh.
        const auto gpu = [](uint32_t h) { return 13.5 * (0.15 + 0.85 * double(h) * h / (864.0 * 864.0)); };
        const auto frame = [&](uint32_t h) { return std::max(1000.0 / 60, gpu(h) + 4.0); };
        dr::Controller c;
        uint64_t now = 1'000'000'000ull, busy = 1;
        bool changed = false;
        c.Update(now, busy, 1080, 576, 1000.0 / 60, changed);
        c.height = 864;
        uint32_t h = 864, highest = 0;
        for (double t = 0; t < 10; t += frame(h) / 1000) {
            h = c.Update(now, busy, 1080, 576, 1000.0 / 60, changed);
            busy += uint64_t(gpu(h) * 1e6);
            now += uint64_t(frame(h) * 1e6);
        }
        CHECK(h < 864 && h >= 792);      // one rung or two, not a jump to the floor
        CHECK(frame(h) <= 1000.0 / 60 + 0.01);
        CHECK(c.lastSlow);
        for (double t = 0; t < 80; t += frame(h) / 1000) {
            h = c.Update(now, busy, 1080, 576, 1000.0 / 60, changed);
            highest = std::max(highest, h);
            busy += uint64_t(gpu(h) * 1e6);
            now += uint64_t(frame(h) * 1e6);
        }
        CHECK(highest < 864);            // held off the slow height (the normal 20 s ceiling would have retried it)
        // CPU-bound: 20 ms frames with the GPU busy 9 ms. A lower height would not help: no change.
        dr::Controller d;
        now = 1'000'000'000ull; busy = 1;
        d.Update(now, busy, 1080, 576, 1000.0 / 60, changed);
        d.height = 864;
        for (int f = 0; f < 50 * 20; ++f) {
            h = d.Update(now, busy, 1080, 576, 1000.0 / 60, changed);
            busy += 9'000'000ull;
            now += 20'000'000ull;
        }
        CHECK(h == 864);                 // and no step up into a slow window either
    }
    {
        // Turn 75: a stall (577 ms frames, the GPU mostly idle) is not a scene: no step down.
        dr::Controller d;
        bool changed = false;
        uint64_t now = 1'000'000'000ull, busy = 1;
        d.Update(now, busy, 1080, 576, 1000.0 / 60, changed);
        d.height = 864;
        uint32_t h = 864;
        for (int f = 0; f < 10; ++f) {
            h = d.Update(now, busy, 1080, 576, 1000.0 / 60, changed);
            busy += 76'000'000ull;
            now += 577'000'000ull;
        }
        CHECK(h == 864);
        // GPU-bound at 50 ms a frame is a heavy scene: it still steps down.
        for (int f = 0; f < 100; ++f) {
            h = d.Update(now, busy, 1080, 576, 1000.0 / 60, changed);
            busy += 50'000'000ull;
            now += 50'000'000ull;
        }
        CHECK(h < 864);
    }
    {
        // Turn 75: in a cutscene steps up wait CutsceneUpHoldNs (12 s), steps down do not.
        dr::Controller d;
        bool changed = false;
        uint64_t now = 1'000'000'000ull, busy = 1;
        d.Update(now, busy, 1080, 576, 1000.0 / 60, changed);
        d.height = 576;
        uint32_t h = 576;
        int firstUp = -1;
        for (int f = 0; f < 60 * 30; ++f) {
            h = d.Update(now, busy, 1080, 576, 1000.0 / 60, changed, dr::Rung, 0, dr::Controller::CutsceneUpHoldNs);
            if (changed && firstUp < 0) firstUp = f;
            busy += 6'000'000ull;
            now += 16'666'667ull;
        }
        CHECK(firstUp >= 60 * 12);           // not before 12 s after the last change (3 s outside cutscenes)
        CHECK(h > 576);
    }
    {
        // Turn 83 (Switch 2 docked, 2026-11-20, Canal Street): 22 ms frames, GPU busy 13 ms (59%), a frame in five
        // late. Was 1188 -> 828 rows at the same frame rate; now held. A GPU-bound window still steps down.
        dr::Controller d;
        bool changed = false;
        uint64_t now = 1'000'000'000ull, busy = 1, late = 0;
        d.Update(now, busy, 1440, 720, 1000.0 / 60, changed);
        d.height = 1188;
        uint32_t h = 1188;
        for (int f = 0; f < 45 * 20; ++f) {
            h = d.Update(now, busy, 1440, 720, 1000.0 / 60, changed, dr::Rung, late);
            busy += 13'000'000ull;
            now += 22'000'000ull;
            if (f % 5 == 0) ++late;
        }
        CHECK(h == 1188);
        for (int f = 0; f < 60 * 5; ++f) {
            h = d.Update(now, busy, 1440, 720, 1000.0 / 60, changed, dr::Rung, late);
            busy += 20'000'000ull;
            now += 20'900'000ull;
        }
        CHECK(h < 1188);
    }
    if (failures) return EXIT_FAILURE;
    std::puts("dynamic_resolution_test: ok");
    return 0;
}

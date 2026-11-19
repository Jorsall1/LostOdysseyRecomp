// gpu/dynamic_resolution.h on the Switch 2 range (turn 35): 1080 -> 576 handheld, 1440 -> 1152 docked.
// Build with -D__SWITCH2__ for the Switch 2 Highest (1440).
#include <gpu/dynamic_resolution.h>
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
        // Undocked: the base changes and it starts over at the full 1080.
        bool changed = false;
        CHECK(c.Update(now, busy, 1080, 576, 1000.0 / 60, changed) == 1080);
        CHECK(c.minimum == 576);
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
    if (failures) return EXIT_FAILURE;
    std::puts("dynamic_resolution_test: ok");
    return 0;
}

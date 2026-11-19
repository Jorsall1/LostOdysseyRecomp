#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string_view>

// Dynamic render resolution (the Switch's Graphics → Dynamic resolution, or LO_DYNAMIC_RESOLUTION=1; opt-in): the scene's render height follows the GPU's busy time
// per frame. Above the frame budget it steps down a rung (36 rows), and with room to spare it steps back up, never
// above the configured height nor below the minimum. The presentation pass scales the scene to the output as it does
// for any render height (bicubic by default). Each change resizes the scene targets (a new geometry epoch), so changes
// are spaced out: down after about a second over budget, up after about three seconds of headroom.
namespace gpu::dynamic_resolution
{
    // The default step (LO_DYNAMIC_RESOLUTION_STEP sets another multiple of 9: 16:9 widths stay whole,
    // 540 -> 960, 504 -> 896, ...).
    inline constexpr uint32_t Rung = 36;
    inline constexpr uint32_t Lowest = 360;
    // The tallest fixed rung (settings::SwitchSceneHeights): 1080 on Switch 1, 1440 on Switch 2. Dynamic heights run
    // from Lowest to one row below it, the same bound as resolution::MaxDynamicHeight.
#ifdef __SWITCH2__
    inline constexpr uint32_t Highest = 1440;
#else
    inline constexpr uint32_t Highest = 1080;
#endif
    // Heights resolution::ResolveInternalSize accepts besides its fixed modes.
    inline constexpr bool IsDynamicHeight(uint32_t height)
    {
        return height >= Lowest && height < Highest && height % 9 == 0;
    }
    // The base heights it works from (the Switch's Maximum resolution rows, 360 to 720, and 1080, plus 1440 on
    // Switch 2).
    inline constexpr bool Supports(uint32_t base) { return base >= Lowest && base <= Highest && base % 9 == 0; }
    // The default minimum: 80% of the base height (540 -> 432, 720 -> 576).
    inline constexpr uint32_t DefaultMinimum(uint32_t base) { return std::max(Lowest, base * 4 / 5); }
    // The lowest height it steps down to: whole steps below the base that stay at or above the minimum (and 360).
    inline constexpr uint32_t Floor(uint32_t base, uint32_t minimum, uint32_t step = Rung)
    {
        const uint32_t floorHeight = std::clamp(minimum, Lowest, std::max(base, Lowest));
        return base > floorHeight ? base - (base - floorHeight) / step * step : base;
    }
    // env.txt wins over the settings, for scripted runs: LO_DYNAMIC_RESOLUTION (-1 unset, else 0 or 1),
    // LO_DYNAMIC_RESOLUTION_MIN (0 unset) and LO_DYNAMIC_RESOLUTION_STEP (Rung unset).
    inline int EnvironmentEnabled()
    {
        const char* value = std::getenv("LO_DYNAMIC_RESOLUTION");
        return value && *value ? std::string_view(value) != "0" : -1;
    }
    inline uint32_t EnvironmentMinimum()
    {
        const char* value = std::getenv("LO_DYNAMIC_RESOLUTION_MIN");
        return value && *value ? uint32_t(std::strtoul(value, nullptr, 10)) : 0u;
    }
    inline uint32_t EnvironmentStep()
    {
        const char* value = std::getenv("LO_DYNAMIC_RESOLUTION_STEP");
        return value && *value ? uint32_t(std::strtoul(value, nullptr, 10)) : Rung;
    }

    struct Controller
    {
        // Tuning: the share of the frame budget the GPU may use before stepping down, and the share the next rung up
        // must stay under (its cost scaled by its pixel count) before stepping up.
        static constexpr double DownAt = 0.92, UpBelow = 0.85; // UpBelow 0.80 -> 0.85 (turn 53): one more rung in battle (576 -> 612 at 12.5 ms)
        static constexpr uint64_t WindowNs = 500'000'000ull, DownHoldNs = 1'000'000'000ull, UpHoldNs = 3'000'000'000ull;
        // A frame that costs less than this says nothing about the scene: a loading screen, a fade or a movie (Switch 2,
        // 2026-11-18: 576 -> 612 rows on the loading screen at 0.3 ms before mv_01v, a rebuild of every scene target
        // there and again when the battle started). It never steps up.
        static constexpr double MeaningfulMs = 3.0;
        // The share of a frame's GPU time taken as independent of the scene height when sizing a jump or a step up.
        static constexpr double FixedShare = 0.35;
        // Down jumps size with a smaller fixed share (turn 54, Switch 2 logs 2026-11-19: 35 ms at 1440 -> 10 ms at
        // 720, 40 ms at 1080 -> 10 ms at 576, 19.6 ms at 1440 -> 11.6 ms at 1080: GPU time followed the pixels
        // almost fully). 0.35 sent every one of those straight to the floor.
        static constexpr double DownFixedShare = 0.15;
        // Where a new base starts. Switch 2 (turn 54): 75% of the base, so the title (19 ms at 1440, held there as a
        // light screen at 51 FPS) runs at 1080 and the first scene starts near where it settles (936-1152 rows).
#ifdef __SWITCH2__
        static constexpr double StartShare = 0.75;
#else
        static constexpr double StartShare = 1.0;
#endif
        static constexpr uint32_t StartHeight(uint32_t base, uint32_t minimum, uint32_t step)
        {
            // Whole rungs below the base, like every other height it picks.
            const uint32_t drop = (base - std::min(base, uint32_t(double(base) * StartShare)) + step - 1) / step * step;
            return drop && base > drop ? std::max(base - drop, minimum) : base;
        }

        uint32_t base = 0, minimum = 0, height = 0, step = Rung;
        uint64_t windowStart = 0, windowBusy = 0, lastChange = 0;
        uint32_t windowFrames = 0, downStrikes = 0, upStrikes = 0, requestedMinimum = 0;
        double lastBusyMs = 0;

        // Once per CPU frame. busyTotalNs: gpu_busy's running total. Returns the height to render at, and whether it
        // changed this frame (for the log).
        uint32_t Update(uint64_t nowNs, uint64_t busyTotalNs, uint32_t baseHeight, uint32_t minimumHeight, double budgetMs,
                        bool& changed, uint32_t stepRows = Rung)
        {
            changed = false;
            stepRows = std::clamp(stepRows / 9 * 9, 9u, 180u);
            if (baseHeight != base || stepRows != step || minimumHeight != requestedMinimum) {
                // Docked/undocked or reconfigured: start over at the full height.
                base = baseHeight;
                step = stepRows;
                requestedMinimum = minimumHeight;
                minimum = Floor(base, minimumHeight, step);
                const uint32_t start = StartHeight(base, minimum, step);
                changed = height && height != start;
                height = start;
                Restart(nowNs, busyTotalNs);
                lastChange = nowNs;
                return height;
            }
            ++windowFrames;
            if (nowNs - windowStart < WindowNs)
                return height;
            const uint64_t busy = busyTotalNs - windowBusy;
            const uint32_t frames = windowFrames;
            Restart(nowNs, busyTotalNs);
            if (!busy || !frames)
                return height; // no timestamps (yet): keep the height
            const double busyMs = double(busy) / 1e6 / frames;
            lastBusyMs = busyMs;
            if (busyMs > budgetMs * DownAt) {
                upStrikes = 0;
                if (++downStrikes >= 2 && height > minimum && nowNs - lastChange >= DownHoldNs) {
                    // Straight to the height whose pixels fit the budget (GPU time follows the pixel count, the
                    // square of the height), at least one rung. Each change rebuilds the scene targets, so one
                    // jump costs one rebuild where rung by rung cost one a second (Switch 2's first battle,
                    // 2026-11-18: nine rebuilds from 1080 to 432 rows, each a hitch).
                    // Part of the cost does not follow the pixels (shadow maps, fixed-size targets, per-draw work:
                    // ~13 of 31 ms in a 1080-row battle, turn 36), so the pixels are scaled by what is left.
                    const double share = (budgetMs * UpBelow / busyMs - DownFixedShare) / (1.0 - DownFixedShare);
                    const double fit = share > 0 ? double(height) * std::sqrt(share) : 0;
                    const uint32_t target = fit > 0 ? uint32_t(fit) / step * step : 0;
                    const uint32_t steps = std::max(1u, target < height ? (height - target + step - 1) / step : 1u);
                    height = height - std::min(steps * step, height - minimum);
                    Changed(nowNs, changed);
                }
                return height;
            }
            downStrikes = 0;
            if (height < base && busyMs < MeaningfulMs) {
                upStrikes = 0;
            } else if (height < base) {
                const double up = double(height + step) / height;
                if (busyMs * (FixedShare + (1.0 - FixedShare) * up * up) < budgetMs * UpBelow) {
                    if (++upStrikes >= 6 && nowNs - lastChange >= UpHoldNs) {
                        // More than one rung when the pixels alone (no fixed share: the cautious estimate) fit
                        // under the step-up line (turn 54: 720 -> 972 rows took seven steps and 25 s).
                        const double fit = double(height) * std::sqrt(budgetMs * UpBelow / busyMs);
                        const uint32_t target = std::min(base, uint32_t(fit) / step * step);
                        height = std::max(height + step, target);
                        if (height > base) height = base;
                        Changed(nowNs, changed);
                    }
                } else {
                    upStrikes = 0;
                }
            }
            return height;
        }

        // While a load runs: no change, and the frames until then do not count (their GPU time is a loading screen's).
        void Hold(uint64_t nowNs, uint64_t busyTotalNs)
        {
            if (!base) return;
            Restart(nowNs, busyTotalNs);
            downStrikes = upStrikes = 0;
        }
    private:
        void Restart(uint64_t nowNs, uint64_t busyTotalNs)
        {
            windowStart = nowNs;
            windowBusy = busyTotalNs;
            windowFrames = 0;
        }
        void Changed(uint64_t nowNs, bool& changed)
        {
            changed = true;
            lastChange = nowNs;
            downStrikes = upStrikes = 0;
        }
    };
}

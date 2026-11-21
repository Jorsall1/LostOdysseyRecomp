#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>
// Dynamic shadows Low: which on-screen casters keep their shadow (patches/shadow_budget.cpp holds the guest side).
//
// Turn 81 (Switch 2, 2026-11-20, Artists' Salon pot): with only the 8 largest casters kept and a 0.8 keep line,
// a mid-sized caster's shadow went on and off as the player walked, because its rank changed whenever a nearer
// person or object grew past it. The rules now:
//   * MaxCount 12 largest (was 8); off-screen casters still get none, the bulk of the saving (Uhra main street:
//     52 of 60 shadows were for casters off screen).
//   * Enter in the MaxCount largest, keep while in the MaxCount + KeepSlack largest (was: down to 0.8 of the
//     MaxCount-th size), so at most 16 shadows on screen.
//   * A caster's shadow changes at most once per DwellFrames (0.5 s): a size that crosses the line back and forth
//     no longer flickers. A caster seen for the first time (or back on screen) decides at once.
//   * Casters under MinSize (720p pixels, about their height) get none: their shadow is a few pixels.
//   * A caster off screen keeps its shadow OffScreenFrames (0.5 s, was 15 frames), so one at the screen edge whose
//     shadow falls into view does not blink.
namespace gpu::shadow_low
{
inline constexpr size_t MaxCount = 12;
inline constexpr size_t KeepSlack = 4;
inline constexpr float MinSize = 12.0f;
inline constexpr uint32_t DwellFrames = 30;
inline constexpr uint32_t OffScreenFrames = 30;
struct Caster
{
    uint32_t lastOnScreen = 0; // frame
    uint32_t changed = 0;      // frame the shadow last turned on or off
    uint32_t counted = 0;      // frame its size went into the rank list (one entry per caster, not per light)
    bool decided = false;      // has a shadow decision since it came on screen
    bool shadow = false;
};
struct Selector
{
    uint32_t frame = 1;
    // The MaxCount-th / (MaxCount + KeepSlack)-th largest size of the previous frame (0: fewer casters).
    float enterSize = 0.0f, keepSize = 0.0f;
    bool history = false; // the previous frame had casters, so the lines above mean something
    std::vector<float> sizes, lastSizes; // on-screen caster sizes this frame / the previous frame
    // Statistics for the log, reset by the caller.
    uint64_t statFrames = 0, statCasters = 0, statShown = 0;
    uint32_t frameShown = 0;
    // Once per frame, before the casters.
    void BeginFrame()
    {
        if (frame > 1) {
            ++statFrames;
            statCasters += sizes.size();
            statShown += frameShown;
        }
        frameShown = 0;
        ++frame;
        std::swap(sizes, lastSizes);
        sizes.clear();
        enterSize = keepSize = 0.0f;
        history = !lastSizes.empty();
        if (lastSizes.size() > MaxCount + KeepSlack) {
            std::nth_element(lastSizes.begin(), lastSizes.begin() + (MaxCount + KeepSlack - 1), lastSizes.end(),
                std::greater<float>());
            keepSize = lastSizes[MaxCount + KeepSlack - 1];
        }
        if (lastSizes.size() > MaxCount) {
            std::nth_element(lastSizes.begin(), lastSizes.begin() + (MaxCount - 1), lastSizes.end(), std::greater<float>());
            enterSize = lastSizes[MaxCount - 1];
        }
    }
    // Marks the caster seen this frame; returns false when it has been off screen too long (no shadow).
    bool Visible(Caster& caster, bool onScreen)
    {
        if (onScreen) caster.lastOnScreen = frame;
        if (frame - caster.lastOnScreen > OffScreenFrames) {
            caster.shadow = false;
            caster.decided = false; // back on screen: decide at once
            return false;
        }
        return true;
    }
    // A caster that is (or recently was) on screen, at `size`: whether it gets its shadow this frame.
    bool Decide(Caster& caster, float size)
    {
        // A caster lit by two lights comes twice a frame: the second call repeats the first decision.
        if (caster.counted == frame) return caster.shadow;
        caster.counted = frame;
        sizes.push_back(size);
        const bool want = size >= std::max(MinSize, caster.shadow ? keepSize : enterSize);
        if (!history) {
            // First frame of Low (or after an empty frame): no ranking yet. Show it, decide next frame.
            caster.shadow = want;
            if (want) ++frameShown;
            return want;
        }
        if (!caster.decided) {
            caster.decided = true;
            caster.shadow = want;
            caster.changed = frame;
        } else if (want != caster.shadow && frame - caster.changed >= DwellFrames) {
            caster.shadow = want;
            caster.changed = frame;
        }
        if (caster.shadow) ++frameShown;
        return caster.shadow;
    }
};
}

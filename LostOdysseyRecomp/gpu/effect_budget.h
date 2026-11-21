#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
// Effect budget: with dynamic resolution on, the game's heavier optional passes are dropped while the GPU's busy
// time per frame stays above a budget (default 14 ms at 60 FPS: 84% of the frame), so the scene height can stay
// higher instead. Levels, each kept for at least a second:
//   1: motion blur off, depth of field blur off (patches/graphics_effects.cpp, renderer tone-map weight)
//   2: level 1 + Dynamic shadows Low (patches/shadow_budget.cpp: shadows only for the 8 largest casters on screen)
// A level is dropped again after 5 s with the GPU under 70% of the budget (or under the budget with dynamic
// resolution two rungs above its floor; level 2 only under 70%, and at least 30 s after its cut). Settings that already turn an effect off
// are untouched; the budget never turns one on. LO_EFFECT_BUDGET=0 turns it off, LO_EFFECT_BUDGET_MS=<ms> sets the
// budget (0 = 84% of the frame time).
//
// Depth of field needs two sides to agree: the guest's blur draws (skipped) and the renderer's tone-map weight
// (zeroed), which runs a frame or more after the guest built it. Zeroing the weight with the blur still running is
// harmless (the blur is unused), skipping the blur with the weight still on is not (the tone map blends a black
// texture). So the weight goes to zero first and the blur stops DofLagFrames later; on the way back the blur
// starts first and the weight returns DofLagFrames later.
namespace gpu::effect_budget
{
inline constexpr uint32_t MaxLevel = 2;
inline constexpr uint32_t DofLagFrames = 30;

struct Controller
{
    static constexpr uint64_t WindowNs = 500'000'000ull, CutHoldNs = 1'000'000'000ull, RestoreHoldNs = 5'000'000'000ull;
    static constexpr double RestoreShare = 0.70, MeaningfulMs = 3.0;
    static constexpr uint32_t CutWindows = 2, RestoreWindows = 6;
    uint32_t level = 0;
    // CPU frames since the level last crossed 0 <-> 1 (depth of field ordering).
    uint32_t framesSinceDofChange = DofLagFrames;
    uint64_t windowStart = 0, windowBusy = 0, lastChange = 0, lastRestore = 0;
    // A cut soon after a restore doubles the wait before the next restore (5 s up to 80 s), so restoring an effect
    // and cutting it again when it costs more than the rows dynamic resolution can give does not cycle.
    static constexpr uint64_t RecutNs = 30'000'000'000ull, MaxRestoreHoldNs = 80'000'000'000ull;
    uint64_t restoreHoldNs = RestoreHoldNs;
    // Level 2 -> 1 brings every caster's shadow back at once, the most visible change the budget makes. Switch 2
    // handheld, 2026-11-20 (turn 72): shadows went low and full 16 times in 14 minutes, most restores at 11.7-13.4 ms
    // through the two-rung slack and cut again 6-25 s later. That level comes back only with the GPU under 70% of
    // the budget (no slack) and at least ShadowRestoreHoldNs after it was cut.
    static constexpr uint64_t ShadowRestoreHoldNs = 30'000'000'000ull;
    uint32_t windowFrames = 0, cutStrikes = 0, restoreStrikes = 0;
    double lastBusyMs = 0;
    bool started = false;

    // Once per CPU frame. Returns true when the level changed.
    // atFloor: dynamic resolution has no lower height left; cuts wait for it (turn 54: both reacted to the same
    // 35-40 ms spike entering the field, and the effects stayed cut for the session). resolutionSlack: dynamic
    // resolution sits two or more rungs above its floor, so a restore under the budget (not only under 70% of
    // it, which a resolution that climbs to ~85% of the frame never allows) costs rows, not frames.
    // cutBudgetMs (0: budgetMs): the line a cut needs. In a cutscene the caller passes the whole frame time
    // (turn 75, Switch 2 handheld log 2026-11-20, 2x shadows: depth of field went off/on and shadows low/full
    // 10 times inside cutscenes, at 14.1-15.4 ms GPU, all frames still on time; each one a visible pop in a
    // close-up). With cutscene set a restore also needs the GPU under 70% of the budget, never the resolution
    // slack, so a restored effect is not cut again a few seconds later.
    bool Update(uint64_t nowNs, uint64_t busyTotalNs, double budgetMs, bool atFloor = true, bool resolutionSlack = false,
                double cutBudgetMs = 0, bool cutscene = false)
    {
        if (cutBudgetMs < budgetMs) cutBudgetMs = budgetMs;
        if (framesSinceDofChange < DofLagFrames) ++framesSinceDofChange;
        if (!started) {
            started = true;
            Restart(nowNs, busyTotalNs);
            lastChange = nowNs;
            return false;
        }
        ++windowFrames;
        if (nowNs - windowStart < WindowNs) return false;
        const uint64_t busy = busyTotalNs - windowBusy;
        const uint32_t frames = windowFrames;
        Restart(nowNs, busyTotalNs);
        if (!busy || !frames) return false;
        const double busyMs = double(busy) / 1e6 / frames;
        lastBusyMs = busyMs;
        if (busyMs < MeaningfulMs) {
            // A loading screen, fade or movie says nothing about the scene.
            cutStrikes = restoreStrikes = 0;
            return false;
        }
        if (busyMs > cutBudgetMs) {
            restoreStrikes = 0;
            if (!atFloor) {
                cutStrikes = 0;
                return false;
            }
            if (++cutStrikes >= CutWindows && level < MaxLevel && nowNs - lastChange >= CutHoldNs) {
                if (lastRestore && nowNs - lastRestore < RecutNs)
                    restoreHoldNs = std::min(restoreHoldNs * 2, MaxRestoreHoldNs);
                else if (!lastRestore || nowNs - lastRestore >= 2 * RecutNs)
                    restoreHoldNs = RestoreHoldNs; // the last restore held for a minute
                SetLevel(level + 1, nowNs);
                return true;
            }
            return false;
        }
        cutStrikes = 0;
        const bool shadows = level >= 2;
        if (busyMs < budgetMs * RestoreShare || (resolutionSlack && !shadows && !cutscene && busyMs <= budgetMs)) {
            const uint64_t hold = shadows ? std::max(restoreHoldNs, ShadowRestoreHoldNs) : restoreHoldNs;
            if (++restoreStrikes >= RestoreWindows && level > 0 && nowNs - lastChange >= hold) {
                lastRestore = nowNs;
                SetLevel(level - 1, nowNs);
                return true;
            }
        } else {
            restoreStrikes = 0;
        }
        return false;
    }
    // While a load runs: no change, and its frames do not count.
    void Hold(uint64_t nowNs, uint64_t busyTotalNs)
    {
        if (framesSinceDofChange < DofLagFrames) ++framesSinceDofChange;
        Restart(nowNs, busyTotalNs);
        cutStrikes = restoreStrikes = 0;
    }
    void Reset() { *this = {}; }
    bool MotionBlurOff() const { return level >= 1; }
    bool ShadowsLow() const { return level >= 2; }
    // Renderer side: zero at once on the way down, back DofLagFrames after the blur restarted.
    bool DofWeightZero() const { return level >= 1 || framesSinceDofChange < DofLagFrames; }
    // Guest side: skipped only once the weight has been zero for DofLagFrames.
    bool DofBlurSkip() const { return level >= 1 && framesSinceDofChange >= DofLagFrames; }

private:
    void SetLevel(uint32_t next, uint64_t nowNs)
    {
        if ((level == 0) != (next == 0)) framesSinceDofChange = 0;
        level = next;
        lastChange = nowNs;
        cutStrikes = restoreStrikes = 0;
    }
    void Restart(uint64_t nowNs, uint64_t busyTotalNs)
    {
        windowStart = nowNs;
        windowBusy = busyTotalNs;
        windowFrames = 0;
    }
};

inline bool EnvironmentEnabled()
{
    const char* value = std::getenv("LO_EFFECT_BUDGET");
    return !value || std::strcmp(value, "0") != 0;
}
// The budget in ms for a frame of frameMs: LO_EFFECT_BUDGET_MS, else 84% of the frame (14 ms at 60 FPS).
inline double BudgetMs(double frameMs)
{
    const char* value = std::getenv("LO_EFFECT_BUDGET_MS");
    const double fixed = value && *value ? std::strtod(value, nullptr) : 0.0;
    return fixed > 0.0 ? fixed : frameMs * 0.84;
}

// Published by the CPU frame (gpu/frame_plan.cpp), read by the guest hooks and the renderer.
inline std::atomic<bool> g_motionBlurOff{false}, g_shadowsLow{false}, g_dofWeightZero{false}, g_dofBlurSkip{false};
inline std::atomic<uint32_t> g_level{0};
inline void Publish(const Controller& controller)
{
    g_motionBlurOff.store(controller.MotionBlurOff(), std::memory_order_relaxed);
    g_shadowsLow.store(controller.ShadowsLow(), std::memory_order_relaxed);
    g_dofWeightZero.store(controller.DofWeightZero(), std::memory_order_relaxed);
    g_dofBlurSkip.store(controller.DofBlurSkip(), std::memory_order_relaxed);
    g_level.store(controller.level, std::memory_order_relaxed);
}
inline bool MotionBlurOff() { return g_motionBlurOff.load(std::memory_order_relaxed); }
inline bool ShadowsLow() { return g_shadowsLow.load(std::memory_order_relaxed); }
inline bool DofWeightZero() { return g_dofWeightZero.load(std::memory_order_relaxed); }
inline bool DofBlurSkip() { return g_dofBlurSkip.load(std::memory_order_relaxed); }
inline uint32_t Level() { return g_level.load(std::memory_order_relaxed); }
}

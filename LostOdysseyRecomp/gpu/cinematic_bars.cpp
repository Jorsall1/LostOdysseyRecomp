#include <stdafx.h>
#include "cinematic_bars.h"
#include <kernel/memory.h>
#include <movie/movie_player.h>
#include <os/logger.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

extern "C" PPC_FUNC(__imp__sub_828F7840);
extern "C" PPC_FUNC(__imp__sub_82B31720);
extern "C" PPC_FUNC(__imp__sub_82B322D8);
extern "C" PPC_FUNC(__imp__sub_82B31538);
extern "C" PPC_FUNC(__imp__sub_8280DD20);
extern "C" PPC_FUNC(__imp__sub_82374E28);
extern "C" PPC_FUNC(__imp__sub_82810800);

namespace
{
std::mutex mutex;
std::unordered_set<uint32_t> tasks, matinees;
std::atomic<bool> rtActive{false}, pauseActive{false};
bool fadeTail = false; // protected by mutex
bool Object(uint32_t p) { return p >= 0x100000 && p < 0x7C000000 && !(p & 3); }
bool Playing(uint8_t state)
{
    // Same states accepted by the native 823765A8 / 82B322D8 checks.
    return state == 1 || state == 2 || state == 5 || state == 6 || state == 7 || state == 9;
}
bool ScreenFade(uint8_t* base)
{
    const uint32_t fade = PPC_LOAD_U32(0x832C17EC);
    return Object(fade) && (PPC_LOAD_U32(fade + 0x4C) & 0x80000000u);
}
bool ControlRestored(uint8_t* base)
{
    // Scripted field events can enter battle without restoring field controls.
    // The battle is normal gameplay; an independently active Matinee still
    // retains its own owner in the sets below.
    const uint32_t world = PPC_LOAD_U32(0x83318744);
    if (Object(world) && (PPC_LOAD_U32(world + 472) == 3 || PPC_LOAD_U32(world + 476) == 3)) return true;
    const uint32_t engine = PPC_LOAD_U32(0x83315FB4);
    if (!Object(engine)) return false;
    const uint32_t players = PPC_LOAD_U32(engine + 0x2B8), count = PPC_LOAD_U32(engine + 0x2BC);
    if (!Object(players) || !count || count > 4) return false;
    const uint32_t player = PPC_LOAD_U32(players);
    if (!Object(player)) return false;
    const uint32_t controller = PPC_LOAD_U32(player + 0x40);
    return Object(controller) && (PPC_LOAD_U32(controller + 0x5C8) & 0x08000000u);
}
void Publish()
{
    const bool rt = !tasks.empty() || !matinees.empty() || fadeTail;
    if (rtActive.exchange(rt, std::memory_order_relaxed) != rt)
        LOG_INFO("cinematic bars: rt={} tasks={} matinees={} fade_tail={}", rt, tasks.size(), matinees.size(), fadeTail);
}
void Track(std::unordered_set<uint32_t>& set, uint32_t object, bool active, uint8_t* base)
{
    std::lock_guard lock(mutex);
    if (active) set.insert(object);
    else if (set.erase(object))
    {
        fadeTail = true;
        LOG_INFO("cinematic bars: end object={:#x} control={} fade={} transition={}",
            object, ControlRestored(base), ScreenFade(base), PPC_LOAD_U8(0x832CB701));
    }
    Publish();
}
void Matinee(uint32_t object, uint8_t* base)
{
    if (Object(object)) Track(matinees, object, PPC_LOAD_U32(object + 0x5C) && Playing(PPC_LOAD_U8(object + 0x78)), base);
}
}

// Switch 2 New Game hang (2026-11-18, summary_029): the opening Matinee is
// prepared (state 8, +60 bit 0x20000000) but its Play (sub_8280DD20) never
// comes, so its mv_01v movie track is never ticked again and the movie never
// starts. Diagnostics: Matinee state changes, Play calls and script task
// starts are logged (LO_MATINEE_TRACE=0 turns them off). Experiment, off by
// default: LO_MATINEE_FORCE_PLAY=1 calls Play on a Matinee that has sat
// prepared in state 8 for 5 s while a cutscene movie shows no frame.
namespace
{
bool MatineeTrace()
{
    static const bool enabled = [] {
        const char* v = std::getenv("LO_MATINEE_TRACE");
        return !(v && v[0] == '0');
    }();
    return enabled;
}
bool MatineeForcePlay()
{
    static const bool enabled = [] {
        const char* v = std::getenv("LO_MATINEE_FORCE_PLAY");
        return v && v[0] == '1';
    }();
    return enabled;
}
std::mutex traceMutex;
uint32_t traceLines = 0;
constexpr uint32_t kTraceLines = 300;
struct MatineeWatch
{
    uint8_t state = 0xFF;
    std::chrono::steady_clock::time_point preparedSince{};
    bool forced = false;
};
std::unordered_map<uint32_t, MatineeWatch> watches; // protected by traceMutex
}

void gpu::cinematic_bars::UpdatePause(uint8_t* base)
{
    // Both native pause draws (823184B0 / 82376BD8) gate on this byte.
    // This is the game's pause overlay, independent of host Settings/debug UI.
    {
        std::lock_guard lock(mutex);
        // The task can finish before its final screen fade. The native draw
        // clears this active bit when elapsed reaches duration (82306A68).
        if (fadeTail && ControlRestored(base) && !ScreenFade(base)) { fadeTail = false; Publish(); }
    }
    const uint32_t pause = PPC_LOAD_U32(0x832C0C40);
    const uint32_t pauser = Object(pause) ? PPC_LOAD_U32(pause + 12) : 0;
    const bool shown = Object(pause) && PPC_LOAD_U8(pause + 0x5D) && !PPC_LOAD_U8(pause + 0x5E) &&
        Object(pauser) && !PPC_LOAD_U8(pauser + 6);
    if (pauseActive.exchange(shown, std::memory_order_relaxed) != shown)
        LOG_INFO("cinematic bars: pause={} object={:#x}", shown, pause);
}
bool gpu::cinematic_bars::Active()
{
    bool movie = false;
#if !defined(__ANDROID__) && !defined(__SWITCH__) && !defined(__NX__)
    movie = movie::GuestPlaying();
#endif
    return rtActive.load(std::memory_order_relaxed) || pauseActive.load(std::memory_order_relaxed) || movie;
}
bool gpu::cinematic_bars::CutsceneActive()
{
    return rtActive.load(std::memory_order_relaxed);
}

// The scenario request starts its fade BEFORE it starts Matinee. Inspect only
// this call site's already-selected request, once per request, not per frame.
PPC_FUNC(sub_828F7840)
{
    if (uint32_t(ctx.lr) == 0x82AD616C && Object(ctx.r31.u32))
    {
        const uint32_t manager = ctx.r31.u32, array = PPC_LOAD_U32(manager + 20);
        const uint32_t count = PPC_LOAD_U32(manager + 24), id = ctx.r30.u32;
        if (Object(array) && count <= 1024)
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t task = PPC_LOAD_U32(array + 4 * i);
                if (Object(task) && PPC_LOAD_U32(task + 4) == id && PPC_LOAD_U8(task + 1) == 2)
                { Track(tasks, task, true, base); break; }
            }
    }
    __imp__sub_828F7840(ctx, base);
}
PPC_FUNC(sub_82B31720)
{
    const uint32_t task = ctx.r3.u32, lr = uint32_t(ctx.lr);
    const uint8_t before = Object(task) ? PPC_LOAD_U8(task + 1) : 0xFF;
    const bool starting = Object(task) && PPC_LOAD_U8(task + 1) == 2 && !PPC_LOAD_U8(0x832CB774);
    if (starting) Track(tasks, task, true, base);
    __imp__sub_82B31720(ctx, base);
    if (starting && !(ctx.r3.u32 & 0xFF)) Track(tasks, task, false, base);
    if (MatineeTrace())
    {
        // Script tasks (switch2 diagnostics): log each task once per state it is seen in.
        static std::unordered_map<uint32_t, uint8_t> seen;
        std::lock_guard lock(traceMutex);
        auto [it, inserted] = seen.try_emplace(task, before);
        if ((inserted || it->second != before) && traceLines < kTraceLines)
        {
            ++traceLines;
            it->second = before;
            LOG_NOTICE("script task: {:#x} id={:#x} state {} -> {} block={} lr={:#x} result={}", task,
                Object(task) ? PPC_LOAD_U32(task + 4) : 0, before, Object(task) ? PPC_LOAD_U8(task + 1) : 0xFF,
                PPC_LOAD_U8(0x832CB774), lr, ctx.r3.u32 & 0xFF);
        }
    }
}
PPC_FUNC(sub_82B322D8)
{
    const uint32_t task = ctx.r3.u32;
    __imp__sub_82B322D8(ctx, base);
    if (Object(task) && PPC_LOAD_U8(task + 1) == 4 &&
        (PPC_LOAD_U8(task + 3) == 0 || PPC_LOAD_U8(task + 3) == 4))
        Track(tasks, task, false, base);
}
PPC_FUNC(sub_82B31538)
{
    const uint32_t task = ctx.r3.u32;
    __imp__sub_82B31538(ctx, base);
    if (ctx.r3.u32 & 0xFF) Track(tasks, task, false, base);
}
// Direct Matinee playback (including Scenario Jump) need not own an outer task.
// Preparation states 4/8/10/11 never activate bars by themselves.
PPC_FUNC(sub_8280DD20)
{
    const uint32_t object = ctx.r3.u32, lr = uint32_t(ctx.lr);
    const uint8_t before = Object(object) ? PPC_LOAD_U8(object + 0x78) : 0xFF;
    if (Object(object))
    {
        const uint8_t state = before;
        if (state == 1 || state == 8 || state == 9) Track(matinees, object, true, base);
    }
    __imp__sub_8280DD20(ctx, base);
    Matinee(object, base);
    if (MatineeTrace())
    {
        std::lock_guard lock(traceMutex);
        if (traceLines < kTraceLines)
        {
            ++traceLines;
            LOG_NOTICE("matinee: play {:#x} lr={:#x} state {} -> {} flags60={:#x} result={}", object, lr, before,
                Object(object) ? PPC_LOAD_U8(object + 0x78) : 0xFF, Object(object) ? PPC_LOAD_U32(object + 60) : 0,
                ctx.r3.u32 & 0xFF);
        }
    }
}
PPC_FUNC(sub_82374E28)
{
    const uint32_t object = ctx.r3.u32;
    __imp__sub_82374E28(ctx, base);
    Matinee(object, base);
    if (!MatineeTrace() || !Object(object)) return;
    const uint8_t state = PPC_LOAD_U8(object + 0x78);
    const uint32_t flags = PPC_LOAD_U32(object + 60);
    bool force = false;
    {
        std::lock_guard lock(traceMutex);
        auto& watch = watches[object];
        const auto now = std::chrono::steady_clock::now();
        if (watch.state != state)
        {
            if (traceLines < kTraceLines)
            {
                ++traceLines;
                LOG_NOTICE("matinee: {:#x} state {} -> {} flags60={:#x} +84={} +100={} +256={}", object, watch.state,
                    state, flags, PPC_LOAD_U32(object + 84), PPC_LOAD_U32(object + 100), PPC_LOAD_U32(object + 256));
            }
            watch.state = state;
            watch.preparedSince = now;
        }
        if (MatineeForcePlay() && !watch.forced && state == 8 && (flags & 0x20000000u) &&
            now - watch.preparedSince >= std::chrono::seconds(5) && movie::NativeMovieStallMs() >= 5000)
        {
            watch.forced = true;
            force = true;
        }
    }
    if (force)
    {
        LOG_WARNING("matinee: {:#x} prepared for 5 s without Play while the cutscene movie waits; calling Play "
                    "(LO_MATINEE_FORCE_PLAY=1)", object);
        const PPCRegister r3 = ctx.r3;
        const uint64_t lr = ctx.lr;
        ctx.r3.u64 = object;
        sub_8280DD20(ctx, base); // the hooked Play above: bars and trace see it
        ctx.r3 = r3;
        ctx.lr = lr;
    }
}
PPC_FUNC(sub_82810800)
{
    const uint32_t object = ctx.r3.u32;
    __imp__sub_82810800(ctx, base);
    Track(matinees, object, false, base);
}

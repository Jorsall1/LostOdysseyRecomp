#include <stdafx.h>
#include "poll_wait.h"
#include "render_fence.h"
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <unwind.h>
#include <iterator>
#include <utility>
#include <os/logger.h>
#include <kernel/memory.h>
#include <patches/occlusion_history.h>
#include <gpu/renderer.h>

extern "C" PPC_FUNC(__imp__sub_823CF390);
extern "C" PPC_FUNC(__imp__sub_823CF3F0);
extern "C" PPC_FUNC(__imp__sub_82322478);

// kernel/imports.cpp. A fence wait must not keep a guest critical section that
// the rendering thread is asleep on: on Switch 2 hardware (2026-11-16) the
// thread in this loop still owned 0x510610 (the heap allocator's) with
// recursion 422 while the rendering thread sat in RtlEnterCriticalSection at
// 0x823ACD58, so no queued command could run and the fence never fell. Park /
// Relieve / Unpark hand such a section over for the wait and put the depth back
// when it returns.
namespace GuestLockHold
{
    void Park();
    void Unpark(uint32_t thread);
}

// Retain the generated function, including register saves, return values and
// query ordering. Only its repeated not-ready probes can pause on the host.
// The scene renderer's reads of its occlusion queries (sub_823CE960). GetData only agrees once the
// command processor has parsed the query's fence, so the rendering thread slept about 10 ms a frame in a busy street,
// holding up Guest Main behind it. Without host queries every count is made up and never zero, and this site only
// asks zero or not, so the answer is known: one GetData call still kicks the open segment, and if it isn't ready the
// count the command processor will write (0x10000 per record) is stored now. LO_EARLY_QUERY_ANSWER=0 waits again.
static bool AnswerSameFrameRead(PPCContext& ctx, uint8_t* base)
{
    // The six reads in the scene renderer sub_823CE960 (return addresses). The sun flare (sub_823D5430) keeps waiting.
    constexpr uint32_t kReads[] = {0x823CEAD0, 0x823CEC48, 0x823CEEE8, 0x823CEF70, 0x823CF000, 0x823CF134};
    // The count the command processor will write: 0x10000 per record without host queries (the default mode), or
    // xenia's made-up 80-100 (gpu/command_processor.cpp, EVENT_WRITE_ZPD). Other modes keep waiting.
    static const int mode = [] {
        const char* zpd = getenv("LO_ZPD_MODE");
        if (getenv("LO_EARLY_QUERY_ANSWER") && getenv("LO_EARLY_QUERY_ANSWER")[0] == '0') return 0;
        if (!zpd || !*zpd || std::strcmp(zpd, "host") == 0) return 1;
        if (std::strcmp(zpd, "xenia") == 0 || std::strcmp(zpd, "begin0") == 0) return 2;
        return 0;
    }();
    const uint32_t lr = uint32_t(ctx.lr);
    if (!mode || std::find(std::begin(kReads), std::end(kReads), lr) == std::end(kReads) || !gpu::renderer::OcclusionCountsFake())
        return false;
    const uint32_t query = ctx.r3.u32, out = ctx.r4.u32;
    const auto r3 = ctx.r3, r4 = ctx.r4, r5 = ctx.r5, r6 = ctx.r6;
    const uint64_t lrSaved = ctx.lr;
    ctx.r5.u64 = 4;
    ctx.r6.u64 = 1;
    __imp__sub_823CF3F0(ctx, base);
    const int32_t result = ctx.r3.s32;
    ctx.r3 = r3; ctx.r4 = r4; ctx.r5 = r5; ctx.r6 = r6; ctx.lr = lrSaved;
    static std::atomic<uint32_t> answered{0}, early{0};
    if (result == 1 || result < 0)
    {
        PPC_STORE_U32(out, mode == 1 ? 0x10000u * std::max<uint32_t>(1, PPC_LOAD_U32(query + 148)) : 100u);
        early.fetch_add(1, std::memory_order_relaxed);
    }
    if ((answered.fetch_add(1, std::memory_order_relaxed) & 0xFFFFF) == 0xFFFFF)
        LOG_INFO("occlusion: {} same-frame reads, {} answered before the fence", answered.load(), early.load());
    ctx.r3.u64 = 1; // what the loop returns once GetData succeeds
    return true;
}

PPC_FUNC(sub_823CF390)
{
    // Policy::History answers the game's deferred reads without the loop.
    if (occlusion_history::Read(ctx, base)) return;
    if (AnswerSameFrameRead(ctx, base)) return;
    poll_wait::RunScoped(poll_wait::Kind::Query, [&] { __imp__sub_823CF390(ctx, base); });
}

// Loading screens. While the game thread waits on a render command fence
// (FlushRenderingCommands during a map or battle load), the rendering thread
// (RenderingThreadMain 0x824856A0) runs one queued command, then ticks the
// loading-screen movie (0x823B63E0) whenever its tick interval has passed.
// Each tick presents a frame and waits for the GPU to be at most 3 frames
// behind; with presentation paced at 30 fps that wait alone outlasts the
// interval, so the queue drains at about one command per frame and a map load
// takes up to a minute. Measured 2026-10-14: the commands take 3-23 ms in
// total per wait; the ticks take the rest (one tick per command, ~33 ms each,
// tick interval 1/30 s), and the tick's frame is held by the guest's two-vblank
// present interval. render_fence::LoadBoost() makes vblanks faster during such
// a wait (gpu/command_processor.cpp). Pacing at 60 fps alone changed nothing.
// Tested 2026-10-14 17:07: ticks went from ~33 to ~6.4 ms and each batch
// drained ~3x faster, but the game then queued ~3x more commands; the share of
// play time spent in these waits stayed at 23% and the first map load stayed
// at 41 s. The loading wheel flickered, so the boost is opt-in (LO_LOAD_BOOST=1).
// Skipping ticks instead (LO_LOAD_TICK_THROTTLE=1, at most one tick every
// 100 ms between commands) left battle transitions on the loading screen for
// minutes on 2026-10-14, so it is off by default.
namespace
{
    std::atomic<int> g_fenceWaiters{0};
    std::atomic<uint64_t> g_fenceWaitSinceMs{0};
    std::atomic<uint64_t> g_lastMovieTickMs{0};
    std::atomic<uint32_t> g_ticksRun{0}, g_ticksDeferred{0};

    // What the rendering thread does during a fence wait: queued commands
    // (by their Execute function) and loading-screen ticks, in microseconds.
    struct CommandStat { uint32_t execute; uint32_t count; uint64_t us; };
    std::mutex g_statsMutex;
    std::vector<CommandStat> g_commandStats;
    uint64_t g_tickUs = 0, g_tickCount = 0, g_tickMaxUs = 0;
    uint64_t g_commandUs = 0, g_commandCount = 0;
    thread_local std::chrono::steady_clock::time_point t_commandStart;
    thread_local uint32_t t_commandExecute = 0;

    uint64_t ElapsedUs(std::chrono::steady_clock::time_point since)
    {
        return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - since).count());
    }

    std::string TakeRenderThreadStats()
    {
        std::lock_guard lock(g_statsMutex);
        std::sort(g_commandStats.begin(), g_commandStats.end(),
            [](const CommandStat& a, const CommandStat& b) { return a.us > b.us; });
        std::string out = fmt::format("{} commands {:.0f} ms, {} ticks {:.0f} ms (longest {:.1f} ms)",
            g_commandCount, g_commandUs / 1e3, g_tickCount, g_tickUs / 1e3, g_tickMaxUs / 1e3);
        for (size_t i = 0; i < g_commandStats.size() && i < 6; ++i)
            out += fmt::format("; exec {:#x} x{} {:.0f} ms", g_commandStats[i].execute, g_commandStats[i].count,
                g_commandStats[i].us / 1e3);
        g_commandStats.clear();
        g_tickUs = g_tickCount = g_tickMaxUs = g_commandUs = g_commandCount = 0;
        return out;
    }

    uint64_t NowMs()
    {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    bool TickThrottleEnabled()
    {
        static const bool on = [] {
            const char* v = getenv("LO_LOAD_TICK_THROTTLE");
            return v && v[0] == '1';
        }();
        return on;
    }
}

namespace
{
    // Render command ring (FRingBuffer at 0x8336A7A4, read by sub_82485CD8):
    // +0 start, +8 write pointer, +12 wrap/end, +20 read pointer, +24 alignment.
    constexpr uint32_t kRenderRing = 0x8336A7A4;

    struct FenceEvent
    {
        uint64_t ms = 0;
        const char* what = nullptr;
        uint32_t a = 0, b = 0, c = 0, d = 0, pcr = 0;
    };
    struct EventRing
    {
        static constexpr size_t kSize = 32;
        FenceEvent events[kSize];
        uint32_t next = 0;
        std::mutex mutex;
        void Add(const char* what, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t pcr)
        {
            std::lock_guard lock(mutex);
            events[next++ % kSize] = {NowMs(), what, a, b, c, d, pcr};
        }
    };
    EventRing g_fenceEvents;   // enqueue / execute / wait begin / wait end
    EventRing g_commandEvents; // every queued command the rendering thread runs
}

bool render_fence::LoadBoost()
{
    static const bool on = [] {
        const char* v = getenv("LO_LOAD_BOOST");
        return v && v[0] == '1';
    }();
    return on && LongWait();
}

uint32_t render_fence::LoadBoostVblankHz()
{
    static const uint32_t hz = [] {
        const char* v = getenv("LO_LOAD_VBLANK_HZ");
        const unsigned long n = v ? strtoul(v, nullptr, 10) : 0;
        return n >= 60 && n <= 2000 ? uint32_t(n) : 480u;
    }();
    return hz;
}

void render_fence::CommandBegin(uint32_t command)
{
    t_commandExecute = 0;
    if (command >= 0x10000 && command < 0x7C000000)
    {
        // Every queued command, for DumpFenceWaiters: command, vtable, execute, read pointer.
        uint8_t* const base = g_memory.base;
        const uint32_t vtable = PPC_LOAD_U32(command);
        const uint32_t execute = vtable >= 0x82000000 && vtable < 0x84000000 ? PPC_LOAD_U32(vtable + 4) : 0;
        g_commandEvents.Add("command", command, vtable, execute, PPC_LOAD_U32(kRenderRing + 20), 0);
    }
    if (!g_fenceWaitSinceMs.load(std::memory_order_relaxed) || command < 0x10000)
        return;
    uint8_t* const base = g_memory.base;
    const uint32_t vtable = PPC_LOAD_U32(command);
    if (vtable < 0x82000000 || vtable >= 0x84000000)
        return;
    t_commandExecute = PPC_LOAD_U32(vtable + 4);
    t_commandStart = std::chrono::steady_clock::now();
}

void render_fence::CommandEnd()
{
    if (!t_commandExecute)
        return;
    const uint64_t us = ElapsedUs(t_commandStart);
    std::lock_guard lock(g_statsMutex);
    ++g_commandCount;
    g_commandUs += us;
    for (auto& c : g_commandStats)
        if (c.execute == t_commandExecute) { ++c.count; c.us += us; t_commandExecute = 0; return; }
    if (g_commandStats.size() < 256)
        g_commandStats.push_back({ t_commandExecute, 1, us });
    t_commandExecute = 0;
}

bool render_fence::LongWait()
{
    const uint64_t since = g_fenceWaitSinceMs.load(std::memory_order_acquire);
    return since && NowMs() - since >= 250;
}

// Host call stack of a fence waiter (Switch 2 loading hold, turn 19). The guest
// back chain stops at the first function that saves LR through __savegprlr
// (the recompiler drops that store), so the 15 s dump only reached
// 0x82484c24 for [Guest Main]. Every guest function is a host function with
// its own .eh_frame entry, so the host unwinder walks the whole chain. Each
// frame is printed as the guest function that contains it (nearest
// PPCFuncMappings entry below the host PC; overridden functions resolve to
// their wrapper's neighbour, so check them against the raw offset) and the
// offset from _start for addr2line against the build's ELF.
extern "C" void _start();
namespace
{
    struct HostTrace
    {
        uintptr_t pcs[64];
        int count = 0;
    };

    _Unwind_Reason_Code CollectHostFrame(_Unwind_Context* context, void* arg)
    {
        auto* trace = static_cast<HostTrace*>(arg);
        if (trace->count >= int(std::size(trace->pcs)))
            return _URC_END_OF_STACK;
        trace->pcs[trace->count++] = uintptr_t(_Unwind_GetIP(context));
        return _URC_NO_REASON;
    }

    uint32_t GuestFunctionAt(uintptr_t pc)
    {
        static const std::vector<std::pair<uintptr_t, uint32_t>> sorted = [] {
            std::vector<std::pair<uintptr_t, uint32_t>> v;
            for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++)
                if (PPCFuncMappings[i].host)
                    v.emplace_back(reinterpret_cast<uintptr_t>(PPCFuncMappings[i].host), PPCFuncMappings[i].guest);
            std::sort(v.begin(), v.end());
            return v;
        }();
        auto it = std::upper_bound(sorted.begin(), sorted.end(), std::make_pair(pc, UINT32_MAX));
        return it == sorted.begin() ? 0 : std::prev(it)->second;
    }

    std::string FormatHostTrace(const HostTrace& trace)
    {
        const uintptr_t start = reinterpret_cast<uintptr_t>(&_start);
        std::string line;
        for (int i = 0; i < trace.count; ++i)
        {
            const uintptr_t pc = trace.pcs[i];
            line += fmt::format(" {:#x}+{:#x}", GuestFunctionAt(pc - 1), pc - start);
        }
        return line;
    }

    // Turn 19 log (runtime-1795012123454119): no stack line at 12/20/30 s,
    // because Guest Main entered ONE fence wait at about 3.7 s and never left
    // it (the trace was taken on entry only). So each wait now records its
    // host stack on entry, and the watchdog's dump (DumpFenceWaiters) prints
    // every wait still open, with the fence counter it polls, plus the last
    // fence enqueues, fence executions and queued commands on the rendering
    // thread, and the render command ring's pointers.
    struct FenceWaiter
    {
        bool active = false;
        uint64_t sinceMs = 0;
        uint32_t counter = 0, threshold = 0, startValue = 0, pcr = 0, lr = 0;
        HostTrace trace;
    };
    constexpr size_t kFenceWaiterSlots = 4;
    FenceWaiter g_fenceWaiterSlots[kFenceWaiterSlots];
    std::mutex g_fenceWaiterMutex;


    int OpenFenceWaiter(const PPCContext& ctx, uint8_t* base)
    {
        FenceWaiter w;
        w.active = true;
        w.sinceMs = NowMs();
        w.counter = ctx.r3.u32;
        w.threshold = ctx.r4.u32;
        w.startValue = w.counter >= 0x10000 && w.counter < 0xFFFF0000u ? PPC_LOAD_U32(w.counter) : 0;
        w.pcr = ctx.r13.u32;
        w.lr = uint32_t(ctx.lr);
        _Unwind_Backtrace(CollectHostFrame, &w.trace);
        g_fenceEvents.Add("wait begin", w.counter, w.startValue, w.threshold, w.lr, w.pcr);
        std::lock_guard lock(g_fenceWaiterMutex);
        for (size_t i = 0; i < kFenceWaiterSlots; ++i)
            if (!g_fenceWaiterSlots[i].active)
            {
                g_fenceWaiterSlots[i] = w;
                return int(i);
            }
        return -1;
    }

    void CloseFenceWaiter(int slot, uint32_t counter, uint32_t pcr, uint8_t* base)
    {
        uint64_t since = 0;
        {
            std::lock_guard lock(g_fenceWaiterMutex);
            if (slot >= 0)
            {
                since = g_fenceWaiterSlots[slot].sinceMs;
                g_fenceWaiterSlots[slot].active = false;
            }
        }
        const uint32_t value = counter >= 0x10000 && counter < 0xFFFF0000u ? PPC_LOAD_U32(counter) : 0;
        g_fenceEvents.Add("wait end", counter, value, uint32_t(since ? NowMs() - since : 0), 0, pcr);
    }
}

void DumpFenceWaiters()
{
    uint8_t* const base = g_memory.base;
    const uint64_t now = NowMs();
    LOG_WARNING("render command ring 0x{:x}: start={:#x} write={:#x} end={:#x} read={:#x} +16={:#x} +24={:#x} "
                "threaded rendering={}",
        kRenderRing, PPC_LOAD_U32(kRenderRing + 0), PPC_LOAD_U32(kRenderRing + 8), PPC_LOAD_U32(kRenderRing + 12),
        PPC_LOAD_U32(kRenderRing + 20), PPC_LOAD_U32(kRenderRing + 16), PPC_LOAD_U32(kRenderRing + 24),
        PPC_LOAD_U32(0x83318040));
    {
        std::lock_guard lock(g_fenceWaiterMutex);
        bool any = false;
        for (const auto& w : g_fenceWaiterSlots)
        {
            if (!w.active)
                continue;
            any = true;
            const uint32_t value = w.counter >= 0x10000 && w.counter < 0xFFFF0000u ? PPC_LOAD_U32(w.counter) : 0;
            LOG_WARNING("fence wait open {} ms: pcr={:#x} lr={:#x} counter@{:#x} = {} (was {} on entry, waits for <= {}) "
                        "host stack {} frames (guest fn+host offset):{}",
                now - w.sinceMs, w.pcr, w.lr, w.counter, value, w.startValue, w.threshold, w.trace.count,
                FormatHostTrace(w.trace));
        }
        if (!any)
            LOG_WARNING("fence wait open: none");
    }
    for (EventRing* ring : {&g_fenceEvents, &g_commandEvents})
    {
        std::lock_guard lock(ring->mutex);
        LOG_WARNING("last {} (newest last, {} total):", ring == &g_fenceEvents ? "fence events" : "rendering thread commands",
            ring->next);
        const uint32_t count = std::min<uint32_t>(ring->next, EventRing::kSize);
        for (uint32_t i = ring->next - count; i != ring->next; ++i)
        {
            const FenceEvent& e = ring->events[i % EventRing::kSize];
            LOG_WARNING("  -{} ms {} a={:#x} b={:#x} c={:#x} d={:#x} pcr={:#x}", now - e.ms, e.what ? e.what : "?", e.a,
                e.b, e.c, e.d, e.pcr);
        }
    }
}

// Fence enqueue (sub_822D6BB0, r3 = &counter): counter += 1, then an 8-byte
// command {vtable 0x8200112C, &counter} on the ring when threaded rendering is
// on (else counter -= 1 at once). Logged: counter, write pointer before and
// after, read pointer, counter after.
extern "C" PPC_FUNC(__imp__sub_822D6BB0);
PPC_FUNC(sub_822D6BB0)
{
    const uint32_t counter = ctx.r3.u32, pcr = ctx.r13.u32;
    const uint32_t writeBefore = PPC_LOAD_U32(kRenderRing + 8);
    __imp__sub_822D6BB0(ctx, base);
    g_fenceEvents.Add("enqueue", counter, writeBefore, PPC_LOAD_U32(kRenderRing + 8),
        counter >= 0x10000 ? PPC_LOAD_U32(counter) : 0, pcr);
}

// Fence command execute (vtable 0x8200112C slot 1, rendering thread): atomic
// decrement of *(this + 4). Logged: command, counter, counter after, read pointer.
extern "C" PPC_FUNC(__imp__sub_823B6650);
PPC_FUNC(sub_823B6650)
{
    const uint32_t command = ctx.r3.u32, pcr = ctx.r13.u32;
    const uint32_t counter = command >= 0x10000 ? PPC_LOAD_U32(command + 4) : 0;
    __imp__sub_823B6650(ctx, base);
    g_fenceEvents.Add("execute", command, counter, counter >= 0x10000 ? PPC_LOAD_U32(counter) : 0,
        PPC_LOAD_U32(kRenderRing + 20), pcr);
}

// FRenderCommandFence::Wait: this guest loop repeatedly invokes Sleep(0) while
// a shared value exceeds its threshold. Keep its original reads and comparison;
// opt in only this call tree. Guest thread exit uses longjmp: no objects with
// destructors here.
PPC_FUNC(sub_82322478)
{
    const uint32_t fenceCounter = ctx.r3.u32, fencePcr = ctx.r13.u32;
    const int fenceSlot = OpenFenceWaiter(ctx, base);
    if (g_fenceWaiters.fetch_add(1, std::memory_order_acq_rel) == 0)
        g_fenceWaitSinceMs.store(NowMs(), std::memory_order_release);
    GuestLockHold::Park();
    poll_wait::RunScoped(poll_wait::Kind::SharedValue, [&] { __imp__sub_82322478(ctx, base); });
    GuestLockHold::Unpark(uint32_t(ctx.r13.u32));
    CloseFenceWaiter(fenceSlot, fenceCounter, fencePcr, base);
    if (g_fenceWaiters.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        const uint64_t since = g_fenceWaitSinceMs.exchange(0, std::memory_order_acq_rel);
        const uint32_t run = g_ticksRun.exchange(0), deferred = g_ticksDeferred.exchange(0);
        const std::string stats = TakeRenderThreadStats();
        if (since && NowMs() - since >= 1000)
        {
            static bool loggedIntervals = false;
            if (!loggedIntervals)
            {
                // RenderingThreadMain's tick intervals (seconds): idle, between commands.
                loggedIntervals = true;
                uint8_t* const base = g_memory.base;
                const uint32_t idle = PPC_LOAD_U32(0x822185B8), busy = PPC_LOAD_U32(0x822185B4);
                float fIdle, fBusy;
                std::memcpy(&fIdle, &idle, 4);
                std::memcpy(&fBusy, &busy, 4);
                LOG_INFO("rendering thread tick interval: idle {} s, between commands {} s", fIdle, fBusy);
            }
            LOG_INFO("render fence wait {:.1f} s: loading-screen ticks run {}, deferred {}; rendering thread: {}",
                (NowMs() - since) / 1000.0, run, deferred, stats);
        }
    }
}

extern "C" PPC_FUNC(__imp__sub_823B63E0);
PPC_FUNC(sub_823B63E0)
{
    // Only the call between two queued commands (not the idle-loop tick).
    if (uint32_t(ctx.lr) == 0x824857A4)
    {
        const uint64_t since = g_fenceWaitSinceMs.load(std::memory_order_acquire);
        const uint64_t now = NowMs();
        if (TickThrottleEnabled() && since && now - since >= 250 &&
            now - g_lastMovieTickMs.load(std::memory_order_relaxed) < 100)
        {
            g_ticksDeferred.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_ticksRun.fetch_add(1, std::memory_order_relaxed);
    }
    const bool measure = g_fenceWaitSinceMs.load(std::memory_order_relaxed) != 0;
    const auto start = std::chrono::steady_clock::now();
    __imp__sub_823B63E0(ctx, base);
    g_lastMovieTickMs.store(NowMs(), std::memory_order_relaxed);
    if (measure)
    {
        const uint64_t us = ElapsedUs(start);
        std::lock_guard lock(g_statsMutex);
        ++g_tickCount;
        g_tickUs += us;
        if (us > g_tickMaxUs) g_tickMaxUs = us;
    }
}

// GpuPoll for the title GPU timestamp wait lives in debug/gpu_wait_trace.cpp.
// That file already owns PPC_FUNC(sub_823B62A0) and PPC_FUNC(sub_827B6278).

// Loading screen begin / end (Switch 2 loading hold, 2026-11-18). sub_82485920
// sets the loading flag *(0x83318040) = 1 that keeps the rendering thread in
// its loading-screen loop (sub_824856A0); sub_82485A08 flushes the rendering
// commands and clears it. Both are called from sub_8235A9C0 and from the
// 0x827C6A50 / 0x827C8018 paths. A hang with a begin and no end means the
// code that ends the load never ran; the callers name which path started it.
extern "C" PPC_FUNC(__imp__sub_82485920);
extern "C" PPC_FUNC(__imp__sub_82485A08);
static void LogLoadingScreen(const char* what, PPCContext& ctx)
{
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) >= 64)
        return;
    uint8_t* const base = g_memory.base;
    LOG_WARNING("loading screen {}: caller lr={:#x} pcr={:#x} flag={}", what, uint32_t(ctx.lr), ctx.r13.u32,
        PPC_LOAD_U32(0x83318040));
}
PPC_FUNC(sub_82485920)
{
    LogLoadingScreen("begin", ctx);
    __imp__sub_82485920(ctx, base);
}
PPC_FUNC(sub_82485A08)
{
    LogLoadingScreen("end", ctx);
    __imp__sub_82485A08(ctx, base);
}

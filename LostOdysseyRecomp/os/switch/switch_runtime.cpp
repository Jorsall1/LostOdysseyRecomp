// Nintendo Switch process setup, crash log and system dialogs.
// Parts follow UnleashedRecomp-NX (os/switch/runtime_switch.cpp, crash_switch.cpp).
#include <os/platform.h>
#if LO_PLATFORM_SWITCH
#include <os/debug_server.h>
#include <os/netlog.h>
#include <os/switch_platform.h>
#include <os/switch_cpu_profiler.h>
#include <os/suspend_clock.h>
#include <switch.h>
#include <switch/runtime/env.h>
#include <sys/iosupport.h>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cxxabi.h>
#include <exception>
#include <new>
#include <typeinfo>
#include <cstdlib>
#include <malloc.h>
#include <reent.h>
extern "C" void __malloc_lock(struct _reent*); // newlib: the allocator lock, recursive
#include <system_error>
#include <unistd.h>

#ifdef LO_SWITCH_PGO_GEN
namespace os::pgo { void StartAutoSave(); void SaveOnExit(); } // os/switch/pgo_switch.cpp: training builds only
#endif

extern "C"
{
    // A full application (title takeover) gets all the memory and every
    // syscall the 4 GiB guest window needs. __nx_heap_size 0: libnx gives the
    // newlib heap everything that is left; guest memory is carved out of it.
    // Zero means "no cap": libnx sizes the heap from svcGetInfo as
    // TotalMemorySize - UsedMemorySize - 2 MiB (rounded down to 2 MiB, floor
    // 512 MiB), so on Switch 2 the pool is the whole application memory region.
    // Only set this to a number to shrink the heap, never to grow it.
    u32 __nx_applet_type = AppletType_Application;
    size_t __nx_heap_size = 0;

    // libnx calls __libnx_exception_handler on this stack after a CPU fault.
    alignas(16) u8 __nx_exception_stack[0x8000];
    u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
}

// libnx's bounds of newlib's heap (sbrk hands out memory between them).
extern "C" char* fake_heap_start;
extern "C" char* fake_heap_end;

// os/switch/env_file.cpp: applies env.txt again and logs it.
namespace os::switch_platform { void ApplyEnvFile(const char* path); }

// os/log_file.h (not included: this file sees libnx headers, not the PCH).
namespace os::logger { void DrainForExit() noexcept; void CloseForExit() noexcept; }

// Declared at file scope, not inside the anonymous namespace below: their
// definitions live in other translation units and this file cannot include their
// headers (libnx's Mutex and Event typedefs collide with the runtime's, and SDL
// is not available here).
namespace settings::quit_action { bool PushQuit(); } // settings/quit_action_hook.cpp: queues SDL_EVENT_QUIT
namespace apu { void SetPaused(bool paused); }        // apu/audio.cpp: silences the SDL audio device
namespace apu { void StopDriverForExit(); }           // apu/audio.cpp: ends the driver thread, closes the device
namespace perf_stats { void StopMonitorForExit(); }   // debug/perf_stats.cpp: joins the perf: line thread
namespace apu::xma { void Shutdown(); }               // apu/xma.cpp: stops the decoder worker
void DumpGuestThreadStates();                         // kernel/imports.cpp: what every guest thread waits on
void DumpHangWatch();                                 // kernel/imports.cpp: the watched objects and their state
void DumpGuestHotStacks();                            // kernel/imports.cpp: stacks of the threads not blocked
void DumpGuestAliasUsage();                           // kernel/imports.cpp: what the C/E views hold
void DumpFenceWaiters();                              // cpu/poll_wait.cpp: open render fence waits, ring, last events
void LogStutterSample(int sample, uint32_t hitches);  // kernel/imports.cpp: the runtime-log header of a stutter sample
namespace perf_stats { uint32_t HitchCount(); }      // debug/perf_stats.cpp: hitches since the start
namespace movie { uint32_t NativeMovieStallMs(); void LogNativeMovieState(); } // movie/movie_hooks.cpp
void LogAppletEvent(const char* what, uint64_t suspendedMs); // kernel/imports.cpp: an applet line in the runtime log

namespace os::switch_platform
{
// Set while the process is ending: a guest thread that reaches a file syscall
// parks instead of entering fsdev, because libnx's application exit unmounts the
// SD card under it (io/file_system.cpp reads this; see EndProcessToHbmenu).
// Defined below, in a .cpp on both sides: no header carries it.
extern std::atomic<bool> g_suppressGuestIo;
// Set by the main thread when it begins the exit (RunExitOnMainThread): a guest
// thread that reaches a wait, a sleep or a file syscall from then on ends there
// (EndThreadForExit). kernel/dispatcher_wait.h, kernel/imports.cpp and
// io/file_system.cpp read it through extern declarations.
extern std::atomic<bool> g_endGuestThreads;
// Set when control passes back to kaiHEN's loader: no thread ends after it.
extern std::atomic<bool> g_loaderHandoff;
[[noreturn]] void EndThreadForExit();
}
extern "C" void __real_svcSleepThread(s64 nano); // the unwrapped sleep (see the wraps at the end)

namespace
{
// stderr (fd 2) goes through this device: the file plus the network log.
FILE* g_stderrFile = nullptr;
devoptab_t g_stderrDevice{};
AppletHookCookie g_operationModeHook{};
std::atomic<bool> g_operationModeChanged{false};
void StartBoostThread(); // below

// HOME -> X: the system asked the game to close (AppletMessage_ExitRequest).
// Set by the applet hook on whichever thread took the message (SDL's event
// pump or the exit watchdog); the watchdog then runs CloseForHomeMenu.
std::atomic<bool> g_homeCloseRequested{false};
void OnAppletEvent(AppletHookType hook, void*)
{
    if (hook == AppletHookType_OnExitRequest)
        g_homeCloseRequested.store(true, std::memory_order_release);
    if (hook == AppletHookType_OnOperationMode)
    {
        g_operationModeChanged.store(true, std::memory_order_release);
        LogAppletEvent(appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld", 0);
    }
    if (hook == AppletHookType_OnFocusState)
    {
        const AppletFocusState focus = appletGetFocusState();
        LogAppletEvent(focus == AppletFocusState_InFocus ? "in focus" :
                       focus == AppletFocusState_OutOfFocus ? "out of focus (library applet)" :
                       "in the background (HOME menu or sleep)", 0);
    }
    if (hook == AppletHookType_OnResume)
        LogAppletEvent("resumed", 0);
    // A game started docked gets the handheld GPU configuration when it is first undocked.
    if (hook == AppletHookType_OnPerformanceMode && appletGetPerformanceMode() == ApmPerformanceMode_Normal)
        StartBoostThread();
}

ssize_t StderrWrite(struct _reent*, void*, const char* data, size_t size)
{
    if (g_stderrFile)
        std::fwrite(data, 1, size, g_stderrFile);
    os::netlog::Write(data, size);
    return ssize_t(size);
}

char g_crashLogPath[512] = "sdmc:/switch/LostOdysseyRecomp/crash.log";
// The report the player is asked for when the game dies on the console. The
// same text still goes to crash.log, which the log-collection tools read.
char g_crashReportPath[512] = "sdmc:/switch/LostOdysseyRecomp/crash_report.txt";

// Writes one chunk of a crash report to the card and does not return before the
// card has it: the raw file session writes with FsWriteOption_Flush, so no
// stdio buffer stands between the text and the file. The stdio fallback (when
// that session is unavailable) is unbuffered and flushed as well.
void AppendCrashReport(const char* text, size_t size)
{
    if (!text || size == 0)
        return;
    if (!os::switch_platform::RawAppend("crash_report.txt", text, size))
    {
        if (FILE* file = std::fopen(g_crashReportPath, "a"))
        {
            std::setvbuf(file, nullptr, _IONBF, 0);
            std::fwrite(text, 1, size, file);
            std::fflush(file);
            std::fclose(file);
        }
    }
    os::switch_platform::RawAppend("crash.log", text, size); // the old name keeps working
}

// RawAppend's own file system session (see switch_platform.h). The requests are libnx's fs.c ones,
// sent directly on this session.
Service g_rawFs{};
Service g_rawSd{};
bool g_rawReady = false;
char g_rawRoot[256] = "/switch/LostOdysseyRecomp";
Mutex g_rawMutex;

Result RawInitialize()
{
#if defined(__SWITCH2__)
    // Under kaiHEN, smGetService("fsp-srv") returns libounce's override: a
    // clone of HEN's own FS proxy root. Every clone shares one FS object, so
    // SetCurrentProcess on it re-identifies HEN's root as this process (kaiHEN
    // hen/README.md: libounce never sends it for that reason). Once this
    // process ended, HEN's next "@Sdcard:/kaihen/bin/game/exefs.nsp" open ran
    // under a dead process ID and failed, so holding R started the stock game
    // until a reboot: every LO exit (Quit, HOME -> X, crash) "killed kaiHEN".
    // A separate SD card file system object from libnx's fs session needs no
    // identity change, and libnx's fsdev unmount does not close it.
    FsFileSystem sd{};
    const Result rc = fsOpenSdCardFileSystem(&sd);
    if (R_FAILED(rc))
        return rc;
    g_rawSd = sd.s;
    g_rawReady = true;
    return 0;
#else
    Result rc = smGetService(&g_rawFs, "fsp-srv");
    if (R_FAILED(rc))
        return rc;
    const uint64_t pidPlaceholder = 0;
    rc = serviceDispatchIn(&g_rawFs, 1, pidPlaceholder, .in_send_pid = true); // SetCurrentProcess
    if (R_SUCCEEDED(rc))
        rc = serviceDispatch(&g_rawFs, 18, .out_num_objects = 1, .out_objects = &g_rawSd); // OpenSdCardFileSystem
    if (R_FAILED(rc))
    {
        serviceClose(&g_rawFs);
        return rc;
    }
    g_rawReady = true;
    return 0;
#endif
}

// The crash report is built here: the handler must not allocate.
char g_crashText[8192];
size_t g_crashLength = 0;
std::atomic<int> g_crashDepth{0};

__attribute__((format(printf, 1, 2))) void CrashAppend(const char* format, ...)
{
    if (g_crashLength + 1 >= sizeof(g_crashText))
        return;
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(g_crashText + g_crashLength, sizeof(g_crashText) - g_crashLength, format, args);
    va_end(args);
    if (length > 0)
        g_crashLength = std::min(sizeof(g_crashText) - 1, g_crashLength + size_t(length));
}
uintptr_t g_moduleBase = 0;
uintptr_t g_moduleEnd = 0;

// The main NRO's text segment, so crash addresses can be resolved against
// LostOdysseyRecomp.elf with addr2line (offset = pc - base).
void CaptureModuleRange()
{
    MemoryInfo info{};
    u32 page = 0;
    const auto self = reinterpret_cast<uintptr_t>(&CaptureModuleRange);
    if (R_SUCCEEDED(svcQueryMemory(&info, &page, self)))
    {
        g_moduleBase = info.addr;
        g_moduleEnd = info.addr + info.size;
    }
}

void WriteAddress(FILE* file, const char* name, uint64_t value)
{
    if (value >= g_moduleBase && value < g_moduleEnd)
        std::fprintf(file, "%-4s %016" PRIx64 "  (elf+%#" PRIx64 ")\n", name, value, value - g_moduleBase);
    else
        std::fprintf(file, "%-4s %016" PRIx64 "\n", name, value);
}

extern "C" void __nx_win_exit(void) __attribute__((weak)); // weak: the SDK's libnx provides it

std::atomic<bool> g_endingProcess{false};

// Mutes the audio device and stops the XMA worker before the process ends, on a
// thread of its own and for at most 250 ms: both calls take a lock the audio
// threads hold while they run, and the exit must not become a hang of its own.
void QuiesceAudio()
{
    static Thread thread;
    static bool started = false;
    if (!started)
    {
        started = true;
        if (R_FAILED(threadCreate(&thread, [](void*) {
                apu::SetPaused(true);
                apu::xma::Shutdown();
            }, nullptr, nullptr, 0x4000, 0x2C, -2)) || R_FAILED(threadStart(&thread)))
            return;
    }
    for (int tenth = 0; tenth < 25; ++tenth)
        svcSleepThread(10'000'000ull);
}

// The last SD card writes of the process, on every exit path, including those
// that never run the std::atexit handlers (ForceExitWithoutTeardown and the
// Switch 1 ExitNow end in svcExitProcess). In this order:
//  1. the runtime log gets its final flush and closes (os::logger::CloseForExit):
//     every later LOG_* call from any thread still alive returns at once, so
//     nothing that logs can reach the card during fsdevUnmountAll, and no
//     thread stopped below can block on a logger lock a crashed thread holds;
//  2. the perf: monitor thread is joined (it waits on a signal, not a sleep);
//  3. the audio driver thread is ended (500 ms bound) and the device closed.
// Each step is idempotent, so the atexit handlers that repeat 2, 3 and 1 on the
// std::exit path do nothing more. Runs once; a second caller returns at once.
void QuiesceForExit()
{
    static std::atomic<bool> done{false};
    if (done.exchange(true, std::memory_order_acq_rel))
        return;
    os::logger::CloseForExit();
    perf_stats::StopMonitorForExit();
    apu::StopDriverForExit();
}
// ---- Leaving for hbmenu under kaiHEN ---------------------------------------
//
// kaiHEN's loader (ounce-hbloader, switch2/clean_main.cpp and entry.S) runs
// this NRO in the donor game's process and takes it back when libnx's exit
// reaches __nx_exit. Three rules of that loader decide whether hbmenu comes
// back or the donor game is ended:
//  1. __nx_exit puts the loader back on the thread that calls it. The loader
//     entered the NRO on the process main thread; resumed on any other thread,
//     it runs hbmenu with the wrong thread's TLS and never pauses the real main
//     thread. Every exit until 2026-11-17 ran std::exit on the GPU owner or the
//     watchdog thread, and the donor game was closed. The exit now always runs
//     on the main thread, which waits in RunServing (os/main_thread.cpp) for
//     either the guest's main thread to return or an exit request.
//  2. Threads the NRO leaves running are paused and "parked", at most 32 for
//     the life of the process. Past that, or for a thread it cannot park, the
//     loader calls svcExitProcess: the donor game ends. Guest threads therefore
//     end themselves at their next wait or sleep (g_endGuestThreads), the
//     main thread waits briefly for them, and pauses whatever is left so that
//     nothing runs while libnx tears down.
//  3. svcExitProcess anywhere ends the donor game, so no Switch 2 exit path
//     calls it any more.
// RetroArch-S2 returns to hbmenu cleanly for the same reason: it leaves from
// main() on the main thread with its threads joined.
//
// Kept from the earlier exits:
//  * appletLockExit() is held from Initialize() until AllowExit(), and is
//    released before the process leaves (am treated an exit with it held as a
//    fatal application error on 2026-11-16 hardware).
//  * No ISelfController::Exit request: under title takeover am applies it to
//    the donor game.
//  * Audio first, so neither audio thread touches the device while the
//    teardown runs; the log closed before the SD card is unmounted.

std::atomic<bool> g_exitServerActive{false};   // the main thread is in ServeExitOnMainThread
std::atomic<bool> g_exitRequested{false};      // a thread asked it to run the exit
Handle g_mainThreadHandle = INVALID_HANDLE;    // set before g_exitServerActive

bool OnMainThread()
{
    return g_mainThreadHandle != INVALID_HANDLE && threadGetCurHandle() == g_mainThreadHandle;
}

void MarkExit(const char* text)
{
    os::switch_platform::RawAppend("exit.log", text, std::strlen(text));
}

// The threads of this process that a libnx-style runtime created (pthreads,
// std::thread, threadCreate), found the way the loader finds them: through
// the ThreadVars at the end of every 0x200-byte TLS slot. See the loader's
// handleNroThreads.
struct ExitThreadVars
{
    u32 magic;
    Handle handle;
    u64 thread;
    u64 reent;
    u64 tls_tp;
};
static_assert(sizeof(ExitThreadVars) == 0x20);
constexpr u32 kThreadVarsMagic = 0x21545624; // "!TV$"
constexpr u64 kTlsSlotSize = 0x200;

struct ThreadCensus
{
    u32 running = 0; // alive and not this thread
    u32 ended = 0;   // ended; the loader closes the handle
    u32 paused = 0;  // paused by this call
};

// The threads RunExit pauses for libnx's teardown. They must be running again
// when the loader takes over: its parkLeftThread pauses each thread itself,
// and Horizon refuses to pause a thread that is already paused (InvalidState,
// KThread::SetActivity), which the loader treats as a thread it cannot park
// and answers with svcExitProcess - the donor game closed with an error on
// the 2026-11-17 23:52 run. ResumeThenReturnToLoader undoes the pauses.
constexpr u32 kMaxPausedThreads = 256;
Handle g_pausedThreads[kMaxPausedThreads];
u32 g_pausedCount = 0;
LoaderReturnFn g_loaderReturn = nullptr;

// libnx's __libnx_exit ends in __nx_exit(0, envGetExitFuncPtr()): this runs
// there, after __appExit (SD card unmounted, services closed), on the main
// thread and the entry stack, right before the loader. Only system calls.
[[noreturn]] void ResumeThenReturnToLoader(int result)
{
    // From here on no thread may end: one ending between the loader's
    // "still running?" check and its pause makes the pause fail, and the
    // loader then ends the donor game. EndThreadForExit blocks instead.
    os::switch_platform::g_loaderHandoff.store(true, std::memory_order_release);
    for (u32 i = 0; i < g_pausedCount; ++i)
        svcSetThreadActivity(g_pausedThreads[i], ThreadActivity_Runnable);
    g_loaderReturn(result);
}

// Counts the other threads; with pause, also pauses every running one. With
// running, also lists the running threads' handles (up to capacity).
ThreadCensus ScanOtherThreads(bool pause, Handle* running = nullptr, u32 capacity = 0)
{
    ThreadCensus census;
    const u64 ownSlot = reinterpret_cast<u64>(armGetTls()) & ~(kTlsSlotSize - 1);
    const Handle self = threadGetCurHandle();
    u64 address = 0;
    for (;;)
    {
        MemoryInfo info{};
        u32 pageInfo = 0;
        if (R_FAILED(svcQueryMemory(&info, &pageInfo, address)))
            break;
        if (info.type == MemType_ThreadLocal && (info.perm & Perm_R))
        {
            for (u64 slot = info.addr; slot + kTlsSlotSize <= info.addr + info.size; slot += kTlsSlotSize)
            {
                if (slot == ownSlot)
                    continue;
                ExitThreadVars vars;
                std::memcpy(&vars, reinterpret_cast<const void*>(slot + kTlsSlotSize - sizeof(vars)), sizeof(vars));
                if (vars.magic != kThreadVarsMagic || vars.handle == INVALID_HANDLE || vars.handle == self)
                    continue;
                u64 threadId = 0;
                if (R_FAILED(svcGetThreadId(&threadId, vars.handle)))
                    continue; // the slot of a thread that is gone
                s32 index = -1;
                if (R_SUCCEEDED(svcWaitSynchronization(&index, &vars.handle, 1, 0)))
                {
                    ++census.ended; // a thread handle is signaled once the thread has ended
                    continue;
                }
                if (running && census.running < capacity)
                    running[census.running] = vars.handle;
                ++census.running;
                if (pause && g_pausedCount < kMaxPausedThreads &&
                    R_SUCCEEDED(svcSetThreadActivity(vars.handle, ThreadActivity_Paused)))
                {
                    g_pausedThreads[g_pausedCount++] = vars.handle;
                    ++census.paused;
                }
            }
        }
        const u64 next = info.addr + info.size;
        if (next <= address)
            break;
        address = next;
    }
    return census;
}

// Set by RunExit after the audio, perf and log threads are stopped: every
// other thread ends at its next condition-variable wait or sleep.
std::atomic<bool> g_endHostThreads{false};

// Condition variables threads are waiting on now, so RunExit can wake them
// (a spurious wake is allowed for every condition variable user). Fixed size,
// no locks: a thread that finds no free slot just waits unregistered.
constexpr int kCondWaitSlots = 128;
std::atomic<_COND_T*> g_condWaits[kCondWaitSlots];

int RegisterCondWait(_COND_T* cond)
{
    for (int i = 0; i < kCondWaitSlots; ++i)
    {
        _COND_T* expected = nullptr;
        if (g_condWaits[i].compare_exchange_strong(expected, cond, std::memory_order_acq_rel))
            return i;
    }
    return -1;
}

void UnregisterCondWait(int slot)
{
    if (slot >= 0)
        g_condWaits[slot].store(nullptr, std::memory_order_release);
}

void WakeRegisteredWaits()
{
    for (auto& slot : g_condWaits)
        if (_COND_T* cond = slot.load(std::memory_order_acquire))
            __libc_cond_broadcast(cond);
}

// The thread running the exit, and the runtime's own named threads (the CPU
// profiler's registry: pipeline workers, GPU threads, profiler, APM, the
// watchdog...). Copied by RunExit before g_endHostThreads is set. Threads of
// libnx, SDL and the GPU driver are not in it and are left to the loader to
// park: libnx's own teardown may still wait on them.
Handle g_exitRunner = INVALID_HANDLE;
os::switch_cpu_profiler::ThreadInfo g_endable[128];
size_t g_endableCount = 0;

// Every thread except the exit runner. The registered ones alone were not
// enough (2026-11-18 01:05 run): the exit watchdog, APM and awake threads are
// libnx threads with no profiler name. They survived, were paused, and ran
// again after libnx's teardown had closed HID and the applet - the watchdog
// polls HID shared memory and the applet every 100 ms - and the process died
// before the loader had written its report. g_endable only names survivors.
bool ShouldEndThisThread()
{
    return g_endHostThreads.load(std::memory_order_acquire) && threadGetCurHandle() != g_exitRunner;
}

// The exit itself. On the Switch 2 it runs on the main thread whenever that
// thread is serving (see rule 1); otherwise where it is called.
[[noreturn]] void RunExit()
{
    char line[192];
    std::snprintf(line, sizeof(line), "exit: running the exit on the %s thread\n",
                  OnMainThread() ? "main" : "calling (main thread not serving)");
    MarkExit(line);
#if defined(__SWITCH2__)
    // Guest threads end at their next wait, sleep or file syscall (rule 2).
    os::switch_platform::g_endGuestThreads.store(true, std::memory_order_release);
#endif
    QuiesceAudio();
    QuiesceForExit(); // final log flush and close, perf monitor and audio driver stopped
    // Not fflush(nullptr): it locks every open file, and a crashed thread may
    // still hold one of those locks. Done before any thread is paused, as a
    // paused thread could hold stderr's lock.
    std::fflush(stdout);
    std::fflush(stderr);
    const bool nextLoadSupported = envHasNextLoad();
    if (nextLoadSupported)
    {
        const Result nextLoad = envSetNextLoad("sdmc:/hbmenu.nro", "\"sdmc:/hbmenu.nro\"");
        std::fprintf(stderr, "[exit] next load sdmc:/hbmenu.nro: %s (0x%x)\n",
                     R_SUCCEEDED(nextLoad) ? "set" : "refused", unsigned(nextLoad));
    }
    else
        std::fprintf(stderr, "[exit] loader has no next-load support; leaving through the ordinary exit\n");
    std::fprintf(stderr, "[exit] leaving through libnx's application exit on the %s thread (applet type %d)\n",
                 OnMainThread() ? "main" : "calling", int(appletGetAppletType()));
    std::fflush(stderr);
#if defined(__SWITCH2__)
    // Host threads too (perf, audio and the log are stopped above): every
    // other thread now ends at its next condition-variable wait or sleep
    // (__wrap___libc_cond_wait, __wrap_svcSleepThread below), so kaiHEN's
    // loader finds nothing it has to park.
    g_exitRunner = threadGetCurHandle();
    g_endableCount = os::switch_cpu_profiler::CopyThreads(g_endable, 128);
    g_endHostThreads.store(true, std::memory_order_release);
    // Up to 1.5 s for them to end; stop early once none has for 200 ms. The
    // waits registered in g_condWaits are woken each pass, so a thread asleep
    // on a condition variable with no timeout ends too.
    WakeRegisteredWaits();
    ThreadCensus census = ScanOtherThreads(false);
    const u32 runningAtStart = census.running;
    u32 lastRunning = census.running;
    // The full 1.5 s unless every thread has ended: threads that sleep for
    // a second at a time (IO Monitor) need most of it.
    for (int tick = 0; tick < 60 && census.running; ++tick)
    {
        svcSleepThread(25'000'000ull);
        WakeRegisteredWaits();
        census = ScanOtherThreads(false);
    }
    (void)lastRunning;

    // Taken, and never released, before any thread is paused: a thread paused
    // inside malloc or free would otherwise hold the allocator lock that
    // libnx's teardown (__nx_gpu_exit, __nx_win_exit) needs. Recursive, so
    // this thread can still allocate. Threads still running wait for it, then
    // are paused below.
    __malloc_lock(_REENT);
    // Nothing else runs from here on: no thread can reach the SD card or a
    // service while libnx tears down. They run again just before the loader
    // takes over (ResumeThenReturnToLoader), so that it can park them.
    census = ScanOtherThreads(true);
    // Name each survivor and where it stopped (offsets into the executable,
    // tools/switch/switch-cpu-profile.py style): each is a thread the loader
    // must park, and one that runs into closed services after the resume is
    // the one that ends the donor game.
    for (u32 i = 0; i < g_pausedCount; ++i)
    {
        const char* name = "(unregistered)";
        for (size_t n = 0; n < g_endableCount; ++n)
            if (g_endable[n].handle == g_pausedThreads[i])
                name = g_endable[n].name;
        ThreadContext context{};
        const Result rc = svcGetThreadContext3(&context, g_pausedThreads[i]);
        const u64 base = os::switch_cpu_profiler::ModuleBase();
        std::snprintf(line, sizeof(line), "exit: still running: %s (handle %#x) pc elf+%#llx lr elf+%#llx (rc %#x)\n",
                      name, unsigned(g_pausedThreads[i]), (unsigned long long)(context.pc.x - base),
                      (unsigned long long)(context.lr - base), unsigned(rc));
        MarkExit(line);
    }
    if (census.paused)
    {
        g_loaderReturn = envGetExitFuncPtr();
        if (g_loaderReturn)
            envSetExitFuncPtr(&ResumeThenReturnToLoader);
        else
            for (u32 i = 0; i < g_pausedCount; ++i) // no loader to return to: undo now
                svcSetThreadActivity(g_pausedThreads[i], ThreadActivity_Runnable);
    }
    std::snprintf(line, sizeof(line),
                  "exit: threads: %u running at the exit request, %u ended, %u still running, %u paused "
                  "(kaiHEN's loader parks up to 32; more ends the donor game)\n",
                  runningAtStart, census.ended, census.running, census.paused);
    MarkExit(line);
#endif
    // The exit lock is released before anything else so am's 15 s
    // forced-termination timer can never start while the teardown runs
    // (libnx: appletUnlockExit must be used before main returns).
    appletUnlockExit();
    MarkExit(nextLoadSupported ? "exit: next load set, exit lock released; std::_Exit -> libnx exit -> loader\n"
                               : "exit: exit lock released; std::_Exit -> libnx exit\n");
    // _Exit, not exit: libnx's own teardown (__libnx_exit: __appExit,
    // __nx_gpu_exit, __nx_win_exit, _serviceGuardCloseAll, then __nx_exit back
    // to the loader) without the C++ static destructors, which would join or
    // lock on the threads just paused. The atexit work is done above.
    std::_Exit(0);
}

// HOME -> X. The player closed the game from the HOME menu, so the process is
// meant to end: the system does that itself as soon as the exit lock is
// released, which is how every homebrew is closed (RetroArch-S2 releases the
// lock in its deinit). Going back to hbmenu here instead, as this path used
// to, kept the process alive while the system waited on the lock: "Please
// wait", then a forced end treated as an application error, and kaiHEN
// needed a reboot. The donor game can be started again afterwards and goes
// straight to hbmenu (kaiHEN: "the second launch and onwards").
// Only what must not be cut off is done first: the runtime log's last lines
// go to the card and close, and guest file I/O stops.
[[noreturn]] void CloseForHomeMenu()
{
    os::switch_platform::g_suppressGuestIo.store(true, std::memory_order_release);
    MarkExit("exit: HOME -> X: closing the game for the system (log closed, exit lock released)\n");
    os::logger::CloseForExit(); // bounded: gives up on a lock a crashed thread holds
    appletUnlockExit();
    // The system ends the process within moments of the unlock. If it has
    // not after 3 s, end it here: the exit was requested and the lock is
    // released, so this is an ordinary close, not an error.
    for (int tenth = 0; tenth < 30; ++tenth)
        svcSleepThread(100'000'000ull);
    MarkExit("exit: HOME -> X: still running 3 s after the unlock; ending the process\n");
    svcExitProcess();
    __builtin_unreachable();
}

// Every exit path ends here: the title's Quit,
// Settings and debug menu "Quit to hbmenu", HOME -> X, and the crash paths.
[[noreturn]] void EndProcessToHbmenu()
{
    // Set before anything else: from here on guest file I/O parks (or ends, see
    // g_endGuestThreads) in io/file_system.cpp instead of reaching fsdev while
    // libnx's exit unmounts the card (item 5 of retroarch_quit_fix.txt).
    os::switch_platform::g_suppressGuestIo.store(true, std::memory_order_release);
#if defined(__SWITCH2__)
    // HOME -> X reached the normal quit first (SDL turned the request into
    // SDL_EVENT_QUIT): the system is waiting for the process to end, so close
    // for it instead of returning to hbmenu.
    if (g_homeCloseRequested.load(std::memory_order_acquire))
        CloseForHomeMenu();
    if (g_exitServerActive.load(std::memory_order_acquire) && !OnMainThread())
    {
        // Rule 1: hand the exit to the main thread, which polls for it every
        // 10 ms and does nothing else, then end this thread: one thread fewer
        // for the loader, and a crashing thread does not stay on the shared
        // exception stack.
        if (!g_exitRequested.exchange(true, std::memory_order_acq_rel))
            MarkExit("exit: requested; handing the exit to the main thread\n");
        os::switch_platform::EndThreadForExit();
    }
#endif
    if (g_endingProcess.exchange(true, std::memory_order_acq_rel))
    {
        // Another thread is already running the exit: nothing left to do here.
#if defined(__SWITCH2__)
        os::switch_platform::EndThreadForExit();
#else
        // Switch 1: wait for it, but not forever, then repeat the handover.
        for (int tenth = 0; tenth < 50; ++tenth)
            svcSleepThread(100'000'000ull);
        if (envHasNextLoad())
            envSetNextLoad("sdmc:/hbmenu.nro", "\"sdmc:/hbmenu.nro\"");
        appletUnlockExit(); // a second unlock is harmless; the first entrant may not have reached it
        svcExitProcess();
        __builtin_unreachable();
#endif
    }
    RunExit();
}

// A native fault or a C library abort() in NVK / Mesa does not always reach
// __libnx_exception_handler, which is where the crash report is written and
// where every other fatal path leaves for hbmenu. The 15:43:20 run on
// 2026-11-17 ended at 2.15 s with stderr.log stopping mid-driver and no [exit]
// line at all: the process was gone without a next load, so the console lost
// hbmenu. Both signals now leave through the same exit as the watchdog and the
// title's Quit: next load, exit lock released, libnx's application exit.
//
// Deliberately does not allocate and does not use stdio before the line is out:
// the faulting thread may hold a stdio or logger lock. write(2) reaches
// StderrWrite (the file plus the network log) without taking stdout's lock.
void OnFatalSignal(int sig)
{
    static std::atomic<int> reported{0};
    if (reported.exchange(sig, std::memory_order_acq_rel) == 0)
    {
        char line[256];
        const int length = std::snprintf(line, sizeof(line),
            "\n=== Lost Odyssey Recomp fatal signal %d (%s), unix time %llu; ending the process so hbmenu comes back\n",
            sig, sig == SIGABRT ? "SIGABRT" : sig == SIGSEGV ? "SIGSEGV" : "signal",
            (unsigned long long)std::time(nullptr));
        if (length > 0)
        {
            const size_t size = std::min(size_t(length), sizeof(line) - 1);
            const ssize_t written = write(STDERR_FILENO, line, size);
            (void)written;
            // Flushed straight to the card, in case the exit below cannot run.
            os::switch_platform::RawAppend("crash_report.txt", line, size);
            os::switch_platform::RawAppend("crash.log", line, size);
        }
    }
    EndProcessToHbmenu();
}

[[noreturn]] void ExitWithLog()
{
#if defined(__SWITCH2__)
    EndProcessToHbmenu();
#else
    os::logger::DrainForExit(); // lines still queued for the log writer
    // Not fflush(nullptr): it locks every open file, and a crashed thread may
    // still hold one of those locks.
    std::fflush(stdout);
    std::fflush(stderr);
    // The report is already on the card (AppendCrashReport). exit(1) then runs
    // the atexit teardown (log drain, session archive) and libnx's own, so the
    // console goes back to hbmenu instead of the loader seeing a vanished
    // process, which is what took the menu down with the game.
    std::exit(1);
    svcExitProcess(); // only if exit() itself gave up
    __builtin_unreachable();
#endif
}

// A failed allocation (NVK, the guest's host heap, a std::vector) aborts through
// std::terminate, which says nothing about running out of memory. Report it.
void OutOfMemoryHandler()
{
    u64 used = 0, total = 0;
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    char text[384];
    const int length = std::snprintf(text, sizeof(text),
        "\n=== Lost Odyssey Recomp out of memory (new_handler)\nunix time %llu\n"
        "memory: process %llu of %llu MiB (heap limit %llu MiB)\n=== end of crash report\n",
        (unsigned long long)std::time(nullptr), (unsigned long long)(used >> 20),
        (unsigned long long)(total >> 20), (unsigned long long)(size_t(fake_heap_end - fake_heap_start) >> 20));
    if (length > 0)
        AppendCrashReport(text, std::min(size_t(length), sizeof(text) - 1));
    os::logger::DrainForExit();
    std::fflush(stderr);
    std::_Exit(1); // exit() would allocate on the way out
}

void WriteAddress(FILE* file, const char* name, uint64_t value);

// Frame-pointer chain from the caller of this function (host code keeps frame
// pointers; the recompiled game code does not, so the chain may stop there).
void WriteBacktrace(FILE* file, uintptr_t frame)
{
    std::fprintf(file, "backtrace:\n");
    for (int depth = 0; depth < 48 && frame && (frame & 7) == 0; ++depth)
    {
        MemoryInfo info{};
        u32 page = 0;
        if (R_FAILED(svcQueryMemory(&info, &page, frame)) || !(info.perm & Perm_R))
            break;
        const auto* record = reinterpret_cast<const uintptr_t*>(frame);
        WriteAddress(file, "  at", record[1]);
        if (record[0] <= frame)
            break;
        frame = record[0];
    }
}

void OnTerminate()
{
    if (FILE* file = std::fopen(g_crashReportPath, "a"))
    {
        const std::time_t now = std::time(nullptr);
        std::fprintf(file, "\n=== std::terminate, %s", std::ctime(&now));
        if (const std::type_info* type = abi::__cxa_current_exception_type())
        {
            int status = 0;
            char* name = abi::__cxa_demangle(type->name(), nullptr, nullptr, &status);
            std::fprintf(file, "exception type: %s\n", status == 0 && name ? name : type->name());
            std::free(name);
            if (auto exception = std::current_exception())
            {
                try { std::rethrow_exception(exception); }
                catch (const std::exception& error) { std::fprintf(file, "what(): %s\n", error.what()); }
                catch (...) {}
            }
        }
        else
        {
            std::fprintf(file, "no active exception (thread/noexcept/abort path)\n");
        }
        std::fprintf(file, "module %016" PRIxPTR "-%016" PRIxPTR "\n", g_moduleBase, g_moduleEnd);
        std::fprintf(file, "%s\n", os::switch_platform::MemorySummary().c_str());
        WriteBacktrace(file, reinterpret_cast<uintptr_t>(__builtin_frame_address(0)));
        std::fclose(file);
    }
    ExitWithLog();
}
}

#if defined(__SWITCH2__)
// _start is libnx's crt0 entry, the NRO's first instruction: crash addresses are
// reported relative to it, the offset addr2line and tools/switch read.
extern "C" void _start(void) __attribute__((weak));

namespace
{
uintptr_t CrashImageBase()
{
    const uintptr_t start = reinterpret_cast<uintptr_t>(&_start);
    return start ? start : g_moduleBase;
}

// ESR_ELx.EC: the exception class the fault was taken for.
const char* ExceptionClassName(uint32_t esr)
{
    switch ((esr >> 26) & 0x3f)
    {
    case 0x00: return "unknown";
    case 0x01: return "trapped WF*";
    case 0x07: return "trapped SIMD/FP access";
    case 0x0E: return "illegal execution state";
    case 0x11: return "SVC from AArch32";
    case 0x15: return "SVC from AArch64";
    case 0x18: return "trapped MSR/MRS/System";
    case 0x1A: return "trapped HVC";
    case 0x1C: return "trapped SMC";
    case 0x20: return "instruction abort from a lower EL";
    case 0x21: return "instruction abort from the same EL";
    case 0x22: return "PC alignment fault";
    case 0x24: return "data abort from a lower EL";
    case 0x25: return "data abort from the same EL";
    case 0x26: return "SP alignment fault";
    case 0x2C: return "trapped floating-point exception";
    case 0x2F: return "SError";
    case 0x30: return "breakpoint from a lower EL";
    case 0x31: return "breakpoint from the same EL";
    case 0x32: return "software step from a lower EL";
    case 0x33: return "software step from the same EL";
    case 0x34: return "watchpoint from a lower EL";
    case 0x35: return "watchpoint from the same EL";
    case 0x38: return "BKPT from AArch32";
    case 0x3C: return "BRK from AArch64";
    default: return "other";
    }
}

// The Switch 2 crash report: what faulted, where, and with the logs flushed, then
// exit(1). The registers go to the card first, because the backtrace and the
// thread dump take locks (svcQueryMemory per frame, every profiler slot) that the
// crash can have left held; a report with just the registers already names the
// fault. Nothing here allocates.
[[noreturn]] void WriteSwitch2CrashReport(ThreadExceptionDump* ctx)
{
    if (g_crashDepth.fetch_add(1) != 0)
    {
        // A second fault: in this handler, or another thread while the first is
        // being reported (they share the exception stack).
        static const char second[] =
            "=== second fault while reporting a crash, report incomplete\n=== end of crash report\n";
        AppendCrashReport(second, sizeof(second) - 1);
        // Not svcExitProcess: it would end the donor game. The first report
        // is already taking the process to hbmenu; end only this thread.
        svcExitThread();
    }

    g_crashLength = 0;
    char thread[48] = "(unnamed)";
    os::switch_cpu_profiler::TryGetCurrentThreadName(thread, sizeof(thread));
    u64 seconds = 0;
    timeGetCurrentTime(TimeType_Default, &seconds);
    const uintptr_t image = CrashImageBase();
    CrashAppend("\n=== Lost Odyssey Recomp crash report, unix time %llu, thread %s\n",
        (unsigned long long)seconds, thread);
    CrashAppend("exception: %s (ESR EC %#x), error_desc %#x, esr %#x, far %016llx, pstate %#x\n",
        ExceptionClassName(ctx->esr), (ctx->esr >> 26) & 0x3f, ctx->error_desc, ctx->esr,
        (unsigned long long)ctx->far.x, ctx->pstate);
    CrashAppend("image %016llx (_start), module %016llx-%016llx\n", (unsigned long long)image,
        (unsigned long long)g_moduleBase, (unsigned long long)g_moduleEnd);
    auto address = [image](const char* name, uint64_t value) {
        if (value >= g_moduleBase && value < g_moduleEnd)
            CrashAppend("%-4s %016llx  (image+%#llx elf+%#llx)\n", name, (unsigned long long)value,
                (unsigned long long)(value - image), (unsigned long long)(value - g_moduleBase));
        else if (value >= image)
            CrashAppend("%-4s %016llx  (image+%#llx)\n", name, (unsigned long long)value,
                (unsigned long long)(value - image));
        else
            CrashAppend("%-4s %016llx\n", name, (unsigned long long)value);
    };
    address("pc", ctx->pc.x);
    address("lr", ctx->lr.x);
    address("sp", ctx->sp.x);
    address("fp", ctx->fp.x);
    for (int i = 0; i < 29; ++i)
    {
        char name[8];
        std::snprintf(name, sizeof(name), "x%d", i);
        address(name, ctx->cpu_gprs[i].x);
    }
    u64 used = 0, total = 0;
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    CrashAppend("memory: process %llu of %llu MiB, heap limit %llu MiB\n",
        (unsigned long long)(used >> 20), (unsigned long long)(total >> 20),
        (unsigned long long)(size_t(fake_heap_end - fake_heap_start) >> 20));

    AppendCrashReport(g_crashText, g_crashLength);
    g_crashLength = 0;

    CrashAppend("backtrace:\n");
    uintptr_t frame = ctx->fp.x;
    for (int depth = 0; depth < 32 && frame && (frame & 7) == 0; ++depth)
    {
        MemoryInfo info{};
        u32 page = 0;
        if (R_FAILED(svcQueryMemory(&info, &page, frame)) || !(info.perm & Perm_R))
            break;
        const auto* record = reinterpret_cast<const uintptr_t*>(frame);
        address("  at", record[1]);
        if (record[0] <= frame)
            break;
        frame = record[0];
    }
    AppendCrashReport(g_crashText, g_crashLength);

    size_t size = 0;
    const char* threads = os::switch_cpu_profiler::DumpThreads("other threads at the crash:\n", size, "crash_report.txt");
    AppendCrashReport(threads, size);
    static const char done[] = "=== end of crash report\n";
    AppendCrashReport(done, sizeof(done) - 1);

    ExitWithLog(); // flushes the logs, then exit(1)
}
}
#endif

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx)
{
#if defined(__SWITCH2__)
    // Switch 2 writes crash_report.txt, flushed chunk by chunk, and ends with
    // exit(1). Everything below is the older path for the other platforms.
    WriteSwitch2CrashReport(ctx);
#endif
    // Runs on __nx_exception_stack. Nothing here allocates or uses stdio until the report is on the
    // card: a crash with an empty crash.log (Uhra, round 6) means the old fopen/fprintf path got stuck.
    if (g_crashDepth.fetch_add(1) != 0)
    {
        // A second fault: in this handler, or another thread while the first is being reported
        // (they share the exception stack). Note it and leave.
        char line[192];
        const int length = std::snprintf(line, sizeof(line), "=== second fault while reporting a crash: pc %016" PRIx64
            " (elf+%#" PRIx64 ") far %016" PRIx64 "\n", ctx->pc.x, ctx->pc.x - g_moduleBase, ctx->far.x);
        if (length > 0)
            os::switch_platform::RawAppend("crash.log", line, std::min(size_t(length), sizeof(line) - 1));
        svcExitProcess();
    }

    g_crashLength = 0;
    char thread[48] = "(unnamed)";
    os::switch_cpu_profiler::TryGetCurrentThreadName(thread, sizeof(thread));
    u64 seconds = 0;
    timeGetCurrentTime(TimeType_Default, &seconds);
    CrashAppend("\n=== Lost Odyssey Recomp crash, unix time %llu, thread %s\n", (unsigned long long)seconds, thread);
    CrashAppend("error_desc %#x  esr %#x  far %016" PRIx64 "  pstate %#x\n",
        ctx->error_desc, ctx->esr, ctx->far.x, ctx->pstate);
    CrashAppend("module %016" PRIxPTR "-%016" PRIxPTR "\n", g_moduleBase, g_moduleEnd);
    auto address = [](const char* name, uint64_t value) {
        if (value >= g_moduleBase && value < g_moduleEnd)
            CrashAppend("%-4s %016" PRIx64 "  (elf+%#" PRIx64 ")\n", name, value, value - g_moduleBase);
        else
            CrashAppend("%-4s %016" PRIx64 "\n", name, value);
    };
    address("pc", ctx->pc.x);
    address("lr", ctx->lr.x);
    address("sp", ctx->sp.x);
    address("fp", ctx->fp.x);
    for (int i = 0; i < 29; ++i)
    {
        char name[8];
        std::snprintf(name, sizeof(name), "x%d", i);
        address(name, ctx->cpu_gprs[i].x);
    }
    // Frame-pointer chain of the host code (the recompiled game code omits
    // frame pointers, so the chain stops at the first guest function).
    CrashAppend("backtrace:\n");
    uintptr_t frame = ctx->fp.x;
    for (int depth = 0; depth < 32 && frame && (frame & 7) == 0; ++depth)
    {
        MemoryInfo info{};
        u32 page = 0;
        if (R_FAILED(svcQueryMemory(&info, &page, frame)) || !(info.perm & Perm_R))
            break;
        const auto* record = reinterpret_cast<const uintptr_t*>(frame);
        address("  at", record[1]);
        if (record[0] <= frame)
            break;
        frame = record[0];
    }

    if (os::switch_platform::RawAppend("crash.log", g_crashText, g_crashLength))
    {
        // Where every other thread was: the crash may be a symptom of one of them.
        size_t size = 0;
        const char* threads = os::switch_cpu_profiler::DumpThreads("other threads at the crash:\n", size, "crash.log");
        os::switch_platform::RawAppend("crash.log", threads, size);
        static const char done[] = "=== end of crash report\n";
        os::switch_platform::RawAppend("crash.log", done, sizeof(done) - 1);
    }
    else if (FILE* file = std::fopen(g_crashLogPath, "a"))
    {
        // No session of our own: the old path, best effort.
        std::fwrite(g_crashText, 1, g_crashLength, file);
        std::fclose(file);
    }
    ExitWithLog();
}

namespace
{
constexpr u32 kHandheldGpu460Memory1331 = 0x92220008;
Thread g_apmThread;
bool g_apmThreadCreated = false;          // g_apmThread holds a thread to close before the next one
std::atomic<bool> g_apmWanted{false};     // StartHandheldGpuBoost was called
std::atomic<bool> g_apmRunning{false};    // ApmThreadMain is running
std::atomic<bool> g_apmDone{false};       // it ran in handheld mode: applied, reverted or not available
std::atomic<bool> g_apmApplied{false};    // the configuration is ours: ExitNow puts g_apmOriginal back
u32 g_apmOriginal = 0;                    // apm's handheld configuration before ours

bool ReadClocks(u32& gpuHz, u32& memoryHz)
{
    gpuHz = memoryHz = 0;
    if (R_FAILED(clkrstInitialize()))
        return false;
    bool ok = true;
    ClkrstSession session{};
    if (R_SUCCEEDED(clkrstOpenSession(&session, PcvModuleId_GPU, 3)))
    {
        ok &= R_SUCCEEDED(clkrstGetClockRate(&session, &gpuHz));
        clkrstCloseSession(&session);
    }
    else
        ok = false;
    if (R_SUCCEEDED(clkrstOpenSession(&session, PcvModuleId_EMC, 3)))
    {
        ok &= R_SUCCEEDED(clkrstGetClockRate(&session, &memoryHz));
        clkrstCloseSession(&session);
    }
    else
        ok = false;
    clkrstExit();
    return ok;
}

// apmSetPerformanceConfiguration returns before pcv applies the clocks, so the
// result is checked by polling. The apm session stays open for the process.
// apm keeps a configuration per performance mode, so the handheld one set here
// also applies after the console is docked and undocked again.
void ApmThreadMain(void*)
{
    struct Done { ~Done() { g_apmRunning.store(false, std::memory_order_release); } } done;
    if (R_FAILED(apmInitialize()))
    {
        std::fprintf(stderr, "[apm] apm is not available; clocks unchanged.\n");
        g_apmDone = true;
        return;
    }
    ApmPerformanceMode mode = ApmPerformanceMode_Invalid;
    if (R_FAILED(apmGetPerformanceMode(&mode)) || mode != ApmPerformanceMode_Normal)
    {
        std::fprintf(stderr, "[apm] Console is not in handheld mode; clocks unchanged until it is undocked.\n");
        return;
    }
    g_apmDone = true;
    u32 original = 0;
    const bool hasOriginal = R_SUCCEEDED(apmGetPerformanceConfiguration(mode, &original));
    u32 gpuBefore = 0, memoryBefore = 0;
    if (!ReadClocks(gpuBefore, memoryBefore))
    {
        std::fprintf(stderr, "[apm] Unable to read clocks through clkrst; clocks unchanged.\n");
        return;
    }
    const u32 configuration = kHandheldGpu460Memory1331;
    if (R_FAILED(apmSetPerformanceConfiguration(mode, configuration)))
    {
        std::fprintf(stderr, "[apm] Configuration 0x%08X is not available on this firmware.\n", configuration);
        return;
    }
    if (hasOriginal)
    {
        g_apmOriginal = original;
        g_apmApplied.store(true, std::memory_order_release);
    }
    u32 gpuAfter = 0, memoryAfter = 0;
    bool memoryStable = true;
    for (int i = 0; i < 10; ++i)
    {
        svcSleepThread(150000000);
        if (!ReadClocks(gpuAfter, memoryAfter))
        {
            memoryStable = false;
            break;
        }
        const u32 delta = memoryAfter > memoryBefore ? memoryAfter - memoryBefore : memoryBefore - memoryAfter;
        if (delta > 20000000)
        {
            memoryStable = false;
            break;
        }
    }
    if (!memoryStable)
    {
        if (hasOriginal)
            apmSetPerformanceConfiguration(mode, original);
        g_apmApplied = false;
        std::fprintf(stderr, "[apm] 0x%08X changed the memory clock (%.1f -> %.1f MHz); reverted to 0x%08X.\n",
            configuration, memoryBefore / 1e6, memoryAfter / 1e6, original);
        return;
    }
    std::fprintf(stderr, "[apm] GPU %.1f -> %.1f MHz, memory %.1f -> %.1f MHz (configuration 0x%08X, was 0x%08X).\n",
        gpuBefore / 1e6, gpuAfter / 1e6, memoryBefore / 1e6, memoryAfter / 1e6, configuration, original);
}

// Once per process in handheld mode; a start in docked mode tries again at the next undock.
void StartBoostThread()
{
    if (!g_apmWanted.load(std::memory_order_acquire) || g_apmDone.load(std::memory_order_acquire) ||
        g_apmRunning.exchange(true, std::memory_order_acq_rel))
        return;
    if (g_apmThreadCreated) // ended: g_apmRunning was false
    {
        threadWaitForExit(&g_apmThread);
        threadClose(&g_apmThread);
        g_apmThreadCreated = false;
    }
    // A libnx thread: it ends on its own; the next start or nothing closes it.
    if (R_SUCCEEDED(threadCreate(&g_apmThread, ApmThreadMain, nullptr, nullptr, 0x10000, 0x3B, -2)))
    {
        g_apmThreadCreated = true;
        if (R_SUCCEEDED(threadStart(&g_apmThread)))
            return;
    }
    g_apmRunning = false;
}

// ExitNow: apm's handheld configuration as the game found it. apm also resets it
// when the process ends (every start reads GPU 307.2 MHz before the boost), so
// this is for the HOME menu between the quit and the end of the process.
void RestoreApmForExit(void (*mark)(const char*))
{
    if (!g_apmApplied.exchange(false, std::memory_order_acq_rel))
        return;
    const Result rc = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, g_apmOriginal);
    char line[160];
    u32 gpuHz = 0, memoryHz = 0;
    if (R_SUCCEEDED(rc) && appletGetOperationMode() == AppletOperationMode_Handheld)
        for (int i = 0; i < 10 && ReadClocks(gpuHz, memoryHz) && gpuHz > 461000000; ++i)
            svcSleepThread(50'000'000);
    std::snprintf(line, sizeof(line), "exit: apm handheld configuration 0x%08X restored (result 0x%x), GPU %.1f MHz, memory %.1f MHz\n",
        g_apmOriginal, rc, gpuHz / 1e6, memoryHz / 1e6);
    mark(line);
}

// Idle dimming and auto-sleep (issue #7). The system dims the screen and then
// sleeps after the player's own settings without input. The game keeps the
// screen on only while nobody needs to touch the controls; SetScreenAwake
// passes the reasons (start-up, a load, the preparation screen, a movie, an
// in-engine cutscene, a debug-server client). While any holds, auto-sleep is
// off and AwakeThreadMain reports the player active every 15 s; a movie or a
// cutscene also sets the media playback state. When one ends, the idle time
// starts again. With LO_SWITCH_AUTO_SLEEP=0 auto-sleep stays off for the whole
// session and nothing else is done.
bool g_autoSleep = true;
Thread g_awakeThread;
bool g_idleTest = false;      // LO_SWITCH_IDLE_SECONDS: debug-server clients do not count
Mutex g_awakeMutex;
uint32_t g_awakeReasons = 0;  // under g_awakeMutex
bool g_mediaPlayback = false; // what the applet has now
bool g_sleepDisabled = false;

const char* AwakeReasonName(uint32_t bit)
{
    using namespace os::switch_platform;
    switch (bit)
    {
    case AwakeStartup: return "start-up";
    case AwakeLoading: return "load";
    case AwakePreparing: return "preparation";
    case AwakeMovie: return "movie";
    case AwakeCutscene: return "cutscene";
    case AwakeDebugClient: return "debug client";
    case AwakeTestWatching: return "test (watching)";
    case AwakeTestBusy: return "test (busy)";
    default: return "?";
    }
}

// idle:sys ReportUserIsActive, as an input does: the screen lights up again and the idle time restarts.
Result ReportUserActive()
{
    Result rc = idlesysInitialize();
    if (R_SUCCEEDED(rc))
    {
        rc = idlesysReportUserIsActive();
        idlesysExit();
    }
    return rc;
}

// While a reason holds, the player counts as active every 15 s. Neither the media playback state nor auto-sleep
// off stops the system dimming the screen (measured on a console set to sleep after 10 min and to sleep while
// playing media: dimmed 31 and 35 s after an idle restart with either one held), and media playback does not stop
// auto-sleep there either.
void AwakeThreadMain(void*)
{
    for (unsigned seconds = 0;; ++seconds)
    {
        svcSleepThread(1'000'000'000);
        mutexLock(&g_awakeMutex);
        const bool held = g_awakeReasons != 0;
        mutexUnlock(&g_awakeMutex);
        if (held && seconds % 15 == 0)
            ReportUserActive();
    }
}

void ApplyAwakeLocked(uint32_t previous, uint32_t reasons)
{
    using namespace os::switch_platform;
    if (!g_autoSleep)
        return;
    // A reason that ends starts the idle time again (idle:sys ReportUserIsActive): the system counts it from the
    // last input, so after a cutscene, movie or load longer than the player's sleep setting the console would dim
    // and sleep at once. Measured with a 20 s dim / 60 s sleep test override, 10 minutes after the last input:
    // dimmed 21 s and asleep about 90 s after a hold ended.
    const bool restart = (previous & ~reasons) != 0;
    const Result idleResult = restart ? ReportUserActive() : 0;
    // Every reason turns auto-sleep off: on a console set to sleep while playing media, the media playback state
    // alone let it sleep during a movie.
    constexpr uint32_t watching = AwakeMovie | AwakeCutscene | AwakeTestWatching;
    const bool media = (reasons & watching) != 0;
    const bool disabled = reasons != 0;
    Result mediaResult = 0, sleepResult = 0;
    if (media != g_mediaPlayback)
    {
        mediaResult = appletSetMediaPlaybackState(media);
        g_mediaPlayback = media;
    }
    if (disabled != g_sleepDisabled)
    {
        sleepResult = appletSetAutoSleepDisabled(disabled);
        g_sleepDisabled = disabled;
    }
    char names[128] = "none";
    size_t length = 0;
    for (uint32_t bit = 1; bit <= AwakeTestBusy; bit <<= 1)
        if (reasons & bit)
            length += std::snprintf(names + length, sizeof(names) - length, "%s%s", length ? ", " : "", AwakeReasonName(bit));
    char idle[48] = "";
    if (restart)
        std::snprintf(idle, sizeof(idle), ", idle time restarted (0x%x)", idleResult);
    std::fprintf(stderr, "[applet] screen kept on: %s (media playback %d, auto-sleep off %d%s%s)\n", names, int(media), int(disabled),
                 idle, R_FAILED(mediaResult) || R_FAILED(sleepResult) ? ", a request failed" : "");
}
}

namespace os::switch_platform
{
// See EndProcessToHbmenu: guest file syscalls park while the process ends
// (io/file_system.cpp reads it through an extern declaration).
std::atomic<bool> g_suppressGuestIo{false};
std::atomic<bool> g_endGuestThreads{false};
std::atomic<bool> g_loaderHandoff{false};
// A guest thread at a wait, sleep or file syscall once the main thread has
// begun the exit. The caller has released every lock it held. svcExitThread,
// not threadExit or pthread_exit: no thread-local destructors or allocator
// calls on the way out. The loader closes the handle and undoes the stack
// mirror of an ended thread, which leaves nothing for it to park.
void EndThreadForExit()
{
    // After ResumeThenReturnToLoader: block where the loader can pause and
    // park this thread, instead of ending under its scan.
    if (g_loaderHandoff.load(std::memory_order_acquire))
        for (;;)
            __real_svcSleepThread(INT64_MAX);
    for (;;)
        svcExitThread();
}

// os/main_thread.cpp, Switch: the main thread, while the guest's main thread
// runs. Does not return: an exit request, or the guest's main thread
// returning, runs the exit here (EndProcessToHbmenu, rule 1).
void ServeExitOnMainThread(const std::atomic<bool>& guestReturned)
{
    g_mainThreadHandle = threadGetCurHandle();
    g_exitServerActive.store(true, std::memory_order_release);
    // Stays the exit thread for the rest of the process (g_exitServerActive is
    // never cleared): a request is never lost, and never runs elsewhere. The
    // guest's main thread returning also ends the process, here.
    while (!guestReturned.load(std::memory_order_acquire) && !g_exitRequested.load(std::memory_order_acquire))
        svcSleepThread(10'000'000ull);
    g_suppressGuestIo.store(true, std::memory_order_release);
    if (!g_endingProcess.exchange(true, std::memory_order_acq_rel))
        RunExit();
    for (;;) // another thread is already running it (an exit before this loop began)
        svcSleepThread(INT64_MAX);
}

// RawAppend without its own session: libnx's SD card file system (fsdev), with
// fsFs* requests only - no stdio, no newlib locks. On the Switch 2 the private
// fsp-srv session never opened, so exit.log and crash_report.txt were never
// written; this is that case.
bool FsdevAppend(const char* fileName, const char* data, size_t size)
{
    FsFileSystem* sd = fsdevGetDeviceFileSystem("sdmc");
    if (!sd)
        return false;
    char path[FS_MAX_PATH]{};
    std::snprintf(path, sizeof(path), "%s/%s", g_rawRoot, fileName);
    fsFsCreateFile(sd, path, 0, 0); // fails harmlessly if it exists
    FsFile file{};
    Result rc = fsFsOpenFile(sd, path, FsOpenMode_Write | FsOpenMode_Append, &file);
    if (R_FAILED(rc))
        return false;
    s64 offset = 0;
    rc = fsFileGetSize(&file, &offset);
    if (R_SUCCEEDED(rc))
        rc = fsFileWrite(&file, offset, data, size, FsWriteOption_Flush);
    fsFileClose(&file);
    return R_SUCCEEDED(rc);
}
bool RawAppend(const char* fileName, const char* data, size_t size)
{
    if (size == 0)
        return false;
    if (!g_rawReady)
        return FsdevAppend(fileName, data, size);
    // The watchdog and the crash handler may both write. Never wait long: a stuck writer must
    // not stop the other one.
    bool locked = false;
    for (int attempt = 0; attempt < 100 && !(locked = mutexTryLock(&g_rawMutex)); ++attempt)
        svcSleepThread(1'000'000);
    char path[0x301]{};
    std::snprintf(path, sizeof(path), "%s/%s", g_rawRoot, fileName);
    const struct { u32 option; u32 pad; s64 size; } create = { 0, 0, 0 };
    serviceDispatchIn(&g_rawSd, 0, create, // CreateFile; fails harmlessly if it exists
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In }, .buffers = { { path, sizeof(path) } });
    Service file{};
    const u32 mode = FsOpenMode_Write | FsOpenMode_Append;
    Result rc = serviceDispatchIn(&g_rawSd, 8, mode, // OpenFile
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In }, .buffers = { { path, sizeof(path) } },
        .out_num_objects = 1, .out_objects = &file);
    if (R_SUCCEEDED(rc))
    {
        s64 offset = 0;
        rc = serviceDispatchOut(&file, 4, offset); // GetSize
        if (R_SUCCEEDED(rc))
        {
            const struct { u32 option; u32 pad; s64 offset; s64 size; } in = { FsWriteOption_Flush, 0, offset, s64(size) };
            rc = serviceDispatchIn(&file, 1, in, // Write
                .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In | SfBufferAttr_HipcMapTransferAllowsNonSecure },
                .buffers = { { data, size } });
        }
        serviceClose(&file);
    }
    if (locked)
        mutexUnlock(&g_rawMutex);
    return R_SUCCEEDED(rc);
}

void StartHandheldGpuBoost()
{
    g_apmWanted.store(true, std::memory_order_release);
    StartBoostThread();
}

std::string ScreenSummary()
{
    mutexLock(&g_awakeMutex);
    const uint32_t reasons = g_awakeReasons;
    const bool media = g_mediaPlayback, disabled = g_sleepDisabled;
    mutexUnlock(&g_awakeMutex);
    char names[128] = "none";
    size_t length = 0;
    for (uint32_t bit = 1; bit <= AwakeTestBusy; bit <<= 1)
        if (reasons & bit)
            length += std::snprintf(names + length, sizeof(names) - length, "%s%s", length ? ", " : "", AwakeReasonName(bit));
    // lbl's dimming flag is set while the system has the screen dimmed.
    bool dimmed = false;
    const bool lbl = R_SUCCEEDED(lblInitialize());
    if (lbl)
    {
        if (R_FAILED(lblIsDimmingEnabled(&dimmed)))
            dimmed = false;
        lblExit();
    }
    char text[256];
    if (!g_autoSleep)
        std::snprintf(text, sizeof(text), "auto-sleep off for the session (LO_SWITCH_AUTO_SLEEP=0)");
    else
        std::snprintf(text, sizeof(text), "kept on: %s (media playback %d, auto-sleep off %d)", names, int(media), int(disabled));
    std::string out = text;
    if (lbl)
        out += dimmed ? "; dimmed now" : "; not dimmed";
    return out;
}

void SetScreenAwake(uint32_t mask, uint32_t reasons)
{
    if (g_idleTest)
        reasons &= ~uint32_t(AwakeDebugClient);
    mutexLock(&g_awakeMutex);
    const uint32_t next = (g_awakeReasons & ~mask) | (reasons & mask);
    if (next != g_awakeReasons)
    {
        const uint32_t previous = g_awakeReasons;
        g_awakeReasons = next;
        ApplyAwakeLocked(previous, next);
    }
    mutexUnlock(&g_awakeMutex);
}

#if defined(__SWITCH2__)
// Below: the watchdog thread (HOME -> X while the game hangs, guest diagnostics).
void StartExitWatchdog();
#endif

void Initialize(const std::filesystem::path& dataRoot)
{
    std::error_code ec;
    std::filesystem::create_directories(dataRoot, ec);
    std::snprintf(g_crashLogPath, sizeof(g_crashLogPath), "%s", (dataRoot / "crash.log").c_str());
    std::snprintf(g_crashReportPath, sizeof(g_crashReportPath), "%s", (dataRoot / "crash_report.txt").c_str());
    {
        const std::string root = dataRoot.string();
        std::snprintf(g_rawRoot, sizeof(g_rawRoot), "%s", root.compare(0, 5, "sdmc:") == 0 ? root.c_str() + 5 : root.c_str());
        mutexInit(&g_rawMutex);
        const Result raw = RawInitialize();
        if (R_FAILED(raw))
        {
            g_rawReady = false;
            std::fprintf(stderr, "[boot] own SD card session unavailable (rc %#x); exit.log and crash reports use libnx's\n",
                         unsigned(raw));
        }
    }
    CaptureModuleRange();
    std::set_terminate(OnTerminate);
    std::set_new_handler(OutOfMemoryHandler); // a failed allocation reports itself

    // Horizon has no console behind stderr: keep the renderer's and the NVK
    // driver's own diagnostics (UnleashedRecomp-NX does the same), and copy
    // them to the network log.
    // One copy of each log, overwritten every launch: the previous session's
    // reports are removed here, before anything of this session can append to
    // them (crash/hang/exit/crash_report append through RawAppend), and
    // stderr.log and early_boot.log are opened with "w".
    for (const char* stale : {"stderr.previous.log", "crash.log", "crash_report.txt", "hang.log", "exit.log",
                              "cpu-samples.bin"})
        std::filesystem::remove(dataRoot / stale, ec);
    g_stderrFile = std::fopen((dataRoot / "stderr.log").c_str(), "w");
    if (g_stderrFile)
        std::setvbuf(g_stderrFile, nullptr, _IONBF, 0);
    g_stderrDevice.name = "stderr";
    g_stderrDevice.write_r = StderrWrite;
    devoptab_list[STD_ERR] = &g_stderrDevice;
    std::setvbuf(stderr, nullptr, _IOLBF, 1024);

    // env.txt: the LO_* switches the PC build reads from its environment. A constructor already applied it before
    // the static initializers (os/switch/env_file.cpp); this pass logs each line and the switches read before it.
    os::switch_platform::ApplyEnvFile((dataRoot / "env.txt").c_str());
    // The network log (LO_NETLOG) and the debug server (LO_DEBUG_SERVER, settings.ini debug_server) share one
    // socket service setup; both stay off, and cost nothing, unless turned on.
    os::debug_server::StartNetwork(dataRoot.string().c_str());
#ifdef LO_SWITCH_PGO_GEN
    // Training build: the profile is saved to the SD card on its own (os/switch/pgo_switch.cpp).
    os::pgo::StartAutoSave();
#endif

    // The HOME menu and sleep suspend the game, as the system does for every game
    // (libnx's SuspendHomeSleep). LO_SWITCH_HOME_SUSPEND=0 keeps it running and
    // rendering behind the HOME menu instead (benchmarks, debug server).
    const char* homeSuspend = getenv("LO_SWITCH_HOME_SUSPEND");
    const bool suspend = !(homeSuspend && std::strcmp(homeSuspend, "0") == 0);
    appletSetFocusHandlingMode(suspend ? AppletFocusHandlingMode_SuspendHomeSleep : AppletFocusHandlingMode_NoSuspend);
    // Idle dimming and auto-sleep follow the system's settings; SetScreenAwake keeps the screen on while nobody
    // needs the controls. LO_SWITCH_AUTO_SLEEP=0 turns auto-sleep off for the session (long unattended runs).
    const char* autoSleep = getenv("LO_SWITCH_AUTO_SLEEP");
    g_autoSleep = !(autoSleep && std::strcmp(autoSleep, "0") == 0);
    mutexInit(&g_awakeMutex);
    if (!g_autoSleep)
        appletSetAutoSleepDisabled(true);
    else if (R_SUCCEEDED(threadCreate(&g_awakeThread, AwakeThreadMain, nullptr, nullptr, 0x4000, 0x3B, -2)))
        threadStart(&g_awakeThread);
    std::fprintf(stderr, "[applet] HOME menu: %s; auto-sleep: %s\n", suspend ? "suspends the game" : "game keeps running (LO_SWITCH_HOME_SUSPEND=0)",
                 g_autoSleep ? "system settings, off during start-up, loads, preparation, movies and cutscenes" : "off (LO_SWITCH_AUTO_SLEEP=0)");
    if (R_SUCCEEDED(setsysInitialize()))
    {
        // The player's System Settings → Sleep Mode, for reports (flags bit 0: sleeps while playing media).
        static const char* const handheld[] = { "1 min", "3 min", "5 min", "10 min", "30 min", "never" };
        SetSysSleepSettings sleep{};
        if (R_SUCCEEDED(setsysGetSleepSettings(&sleep)))
            std::fprintf(stderr, "[applet] sleep settings: handheld plan %d (%s), docked plan %d, %s while playing media (flags 0x%x)\n",
                         sleep.handheld_sleep_plan,
                         sleep.handheld_sleep_plan >= 0 && sleep.handheld_sleep_plan <= 5 ? handheld[sleep.handheld_sleep_plan] : "?",
                         sleep.console_sleep_plan, (sleep.flags & 1) ? "sleeps" : "does not sleep", sleep.flags);
        setsysExit();
    }
    SetScreenAwake(AwakeStartup, AwakeStartup);
    // Test only: LO_SWITCH_IDLE_SECONDS=<dim>,<sleep> overrides the system's idle times for this session
    // (ISelfController OverrideAutoSleepTimeAndDimmingTime: sleep handheld, sleep docked, dim handheld, dim docked);
    // 0,0 keeps the system's. Either way debug-server clients then do not keep the screen on.
    if (const char* idle = getenv("LO_SWITCH_IDLE_SECONDS"))
    {
        int dim = 0, asleep = 0;
        if (std::sscanf(idle, "%d,%d", &dim, &asleep) == 2 && dim >= 0 && asleep >= 0)
        {
            const Result rc = dim > 0 && asleep > 0 ? appletOverrideAutoSleepTimeAndDimmingTime(asleep, asleep, dim, dim) : 0;
            std::fprintf(stderr, "[applet] idle test: dim after %d s, sleep after %d s (0: the system's; result 0x%x); "
                         "debug-server clients do not keep the screen on\n", dim, asleep, rc);
            g_idleTest = true;
        }
    }
    appletLockExit();
    appletHook(&g_operationModeHook, OnAppletEvent, nullptr);
    // NVK's asserts abort() and a driver fault can skip __libnx_exception_handler
    // (see OnFatalSignal). Registered after the exit lock is taken, so the exit
    // below always finds the lock held and releases it exactly once.
    std::signal(SIGABRT, OnFatalSignal);
    std::signal(SIGSEGV, OnFatalSignal);
#if defined(__SWITCH2__)
    StartExitWatchdog();
#endif
}

void AllowExit()
{
    appletUnlockExit();
}

void ExitNow()
{
    // An application must tell the applet manager it is leaving
    // (ISelfController::Exit, as libnx's own application exit does) and then
    // wait to be terminated. Ending the process without it left the screen
    // frozen on the last frame (title Quit and Quit to Desktop, 2026-10-22).
    // The libnx exit teardown is still skipped: it unmounted the SD card under
    // game threads that were reading it.
    // Each step goes to exit.log through the raw file session, so a quit that
    // freezes shows how far it got (the runtime log stops before this).
    auto mark = [](const char* text) { RawAppend("exit.log", text, std::strlen(text)); };
    char line[96];
    std::snprintf(line, sizeof(line), "exit: started (system tick %llu)\n", static_cast<unsigned long long>(armGetSystemTick()));
    mark(line);
#ifdef LO_SWITCH_PGO_GEN
    os::pgo::SaveOnExit(); // training build: the counters since the last save
    mark("exit: profile saved\n");
#endif
    RestoreApmForExit(mark);
    QuiesceForExit(); // final log flush and close, perf monitor and audio driver stopped
    // Not fflush(nullptr): it locks every open file, game files included.
    std::fflush(stdout);
    std::fflush(stderr);
    mark("exit: logs flushed\n");
#if defined(__SWITCH2__)
    // No applet Exit request (see EndProcessToHbmenu): under kaiHEN title
    // takeover it ends the donor game and drops hbmenu.
    mark("exit: ending the process (hbmenu, no applet Exit request)\n");
    EndProcessToHbmenu();
#else
    // libnx closes the default window (vi layer and display) before it sends Exit. Without it the Exit request
    // never returned (exit.log of 2026-10-27 ends at "logs flushed"): close it as libnx does. The renderer has
    // already destroyed its swap chain (video: shutdown stage=window-stop).
    if (&__nx_win_exit)
    {
        __nx_win_exit();
        mark("exit: window closed\n");
    }
    appletUnlockExit();
    // The request on its own thread, so a request that never returns cannot keep the process alive.
    static std::atomic<int> requestState{0}; // 0 sent, 1 returned
    static Result requestResult = 0;
    static Thread requestThread;
    const bool threaded = R_SUCCEEDED(threadCreate(&requestThread, [](void*) {
        requestResult = serviceDispatch(appletGetServiceSession_SelfController(), 0); // Exit
        requestState.store(1, std::memory_order_release);
        for (;;)
            svcSleepThread(UINT64_MAX);
    }, nullptr, nullptr, 0x4000, 0x2C, -2)) && R_SUCCEEDED(threadStart(&requestThread));
    if (!threaded)
    {
        requestResult = serviceDispatch(appletGetServiceSession_SelfController(), 0);
        requestState.store(1, std::memory_order_release);
    }
    bool reported = false;
    for (unsigned tenths = 1; tenths <= 100; ++tenths)
    {
        svcSleepThread(100'000'000ull);
        if (!reported && requestState.load(std::memory_order_acquire) == 1)
        {
            std::snprintf(line, sizeof(line), "exit: applet Exit result 0x%x\n", requestResult);
            mark(line);
            reported = true;
            if (R_FAILED(requestResult))
                break;
        }
        if (tenths == 10 || tenths == 30 || tenths == 100)
        {
            std::snprintf(line, sizeof(line), "exit: still running %u s after the Exit request (%s)\n", tenths / 10,
                          reported ? "answered" : "not answered");
            mark(line);
        }
    }
    mark("exit: ending the process directly (svcExitProcess)\n");
    svcExitProcess();
    __builtin_unreachable();
#endif
}

// The forced end for the paths that used to call std::_Exit directly. libnx's
// application teardown is skipped (guest threads may still be running and were
// the reason those paths used _Exit), but the two handover steps are not: the
// next load, so the loader returns to hbmenu, and the applet exit lock, because
// ending the process with it held is what made am show its fatal application
// error and drop kaiHEN's R -> hbmenu hook on 2026-11-16 hardware.
void ForceExitWithoutTeardown()
{
#if defined(__SWITCH2__)
    // svcExitProcess would end the donor game (see EndProcessToHbmenu, rule 3):
    // take the same exit as a normal quit. It pauses the threads this path
    // used to leave running instead of tearing down under them.
    EndProcessToHbmenu();
#endif
    // No atexit handlers run on this path, so their work is done here: the
    // final log flush and close, the perf monitor and the audio driver.
    QuiesceForExit();
    if (envHasNextLoad())
        envSetNextLoad("sdmc:/hbmenu.nro", "\"sdmc:/hbmenu.nro\"");
    appletUnlockExit();
    std::fflush(stdout);
    std::fflush(stderr);
    svcExitProcess();
    std::_Exit(EXIT_FAILURE); // only if svcExitProcess itself gave up
}

#if defined(__SWITCH2__)
namespace
{
// The watchdog thread: HOME -> X while the game is stuck, and the 15 s / 40 s
// guest diagnostics. It runs on its own libnx thread, so both still work while
// SDL's event thread, the GPU and every guest thread are stuck. (The old
// L + R + Plus + Minus quit combo is gone: L + R opens the debug menu, which
// has Quit to hbmenu.)
Thread g_exitWatchThread;
bool g_exitWatchStarted = false;
constexpr uint64_t kExitWatchPollNs = 100'000'000ull; // 100 ms
// A guest thread dump 15 s in, whether or not the GPU stalled: a screen that
// stops progressing with frames still presenting (the 100% loading bar) leaves
// no other trace of which guest thread is waiting and on what.
constexpr auto kGuestDumpAfter = std::chrono::seconds(15);
constexpr auto kSecondGuestDumpAfter = std::chrono::seconds(40);

uint64_t SteadyNs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void ExitWatchdogMain(void*)
{
    const auto started = std::chrono::steady_clock::now();
    // Debug audit (turn 35): the fixed 15 s / 40 s guest dumps (1.5 s of stack sampling, a C/E scan, synchronous
    // warning lines) ran in every session. LO_GUEST_DUMPS=1 brings them back.
    static const bool fixedDumps = [] {
        const char* v = std::getenv("LO_GUEST_DUMPS");
        return v && v[0] == '1';
    }();
    int guestDumps = fixedDumps ? 0 : 2;
    // A poll that took over 2 s: the process was suspended (HOME menu, sleep mode). os/suspend_clock.h keeps the
    // total so the movie watchdog and the movie status lines leave it out.
    os::suspend_clock::Detector suspend;
    suspend.period = kExitWatchPollNs;
    for (;;)
    {
        svcSleepThread(int64_t(kExitWatchPollNs)); // a kernel sleep: nothing else has to run
        if (const uint64_t suspendedNs = suspend.Observe(SteadyNs()))
        {
            os::suspend_clock::Add(suspendedNs);
            std::fprintf(stderr, "[watchdog] the process was suspended for %llu ms (HOME menu or sleep)\n",
                         (unsigned long long)(suspendedNs / 1'000'000ull));
            LogAppletEvent("suspended, now running again", suspendedNs / 1'000'000ull);
        }
        const auto now = std::chrono::steady_clock::now();
        // The GPU-stall watchdog in gpu/command_processor.cpp only dumps when
        // swaps stop. A fade or loading bar that keeps presenting while the
        // guest waits never reaches it, so dump here as well: at 15 s, and at
        // 40 s, after the objects the first dump added to the hang watch have
        // had 25 s to be signaled. Each pass also samples the stacks of the
        // threads that are not blocked (DumpGuestHotStacks, 1.5 s) and scans
        // the C/E views (DumpGuestAliasUsage).
        if (guestDumps < 2 && now - started >= (guestDumps == 0 ? kGuestDumpAfter : kSecondGuestDumpAfter))
        {
            ++guestDumps;
            if (!g_endHostThreads.load(std::memory_order_acquire) &&
                !g_homeCloseRequested.load(std::memory_order_acquire))
            {
                std::fprintf(stderr, "[watchdog] guest thread states after %d s (frames may still be presenting)\n",
                             guestDumps == 1 ? 15 : 40);
                DumpGuestThreadStates();
                DumpHangWatch();
                DumpGuestHotStacks();
                DumpGuestAliasUsage();
                DumpFenceWaiters();
                std::fprintf(stderr, "[watchdog] guest diagnostics %d done\n", guestDumps);
            }
        }
        // A cutscene movie that has shown no frame 10 s and 30 s after it
        // started (2026-11-18: mv_01v after New Game, 100 s of no picture and
        // no sound while frames kept presenting). Same dump, once each.
        static int movieDumps = 0;
        if (movieDumps < 2 && !g_endHostThreads.load(std::memory_order_acquire) &&
            !g_homeCloseRequested.load(std::memory_order_acquire))
        {
            const uint32_t stallMs = movie::NativeMovieStallMs();
            if (stallMs >= (movieDumps == 0 ? 10000u : 30000u))
            {
                ++movieDumps;
                std::fprintf(stderr, "[watchdog] movie without a frame for %u ms: guest thread states\n", stallMs);
                movie::LogNativeMovieState();
                DumpGuestThreadStates();
                DumpHangWatch();
                DumpGuestHotStacks();
                DumpGuestAliasUsage(); // heap backend: C/E are not aliases of A (the main Switch 1 difference)
                DumpFenceWaiters();
                movie::LogNativeMovieState();
                std::fprintf(stderr, "[watchdog] movie diagnostics %d done\n", movieDumps);
            }
        }
        // Stutter (2026-11-18, first battle: stretches of ~47 ms frames with the
        // command processor idle 43 ms of each, so the guest was late, not the
        // GPU): when 8 or more hitches come within 3 s, sample the guest stacks
        // (1.5 s), the fence waits and the C/E views. Three times at most, 30 s
        // apart. Off by default since turn 35 (the sampling itself made hitches
        // in a GPU-bound battle); LO_STUTTER_SAMPLES=1 turns it on.
        static const bool stutterSamples = [] {
            const char* v = std::getenv("LO_STUTTER_SAMPLES");
            return v && v[0] == '1';
        }();
        static int stutterDumps = 0;
        static uint32_t hitchMark = 0;
        static auto hitchMarkAt = now, lastStutterDump = now - std::chrono::seconds(30);
        if (stutterSamples && stutterDumps < 3 && !g_endHostThreads.load(std::memory_order_acquire) &&
            !g_homeCloseRequested.load(std::memory_order_acquire))
        {
            const uint32_t hitches = perf_stats::HitchCount();
            if (now - hitchMarkAt >= std::chrono::seconds(3))
            {
                hitchMark = hitches;
                hitchMarkAt = now;
            }
            else if (hitches - hitchMark >= 8 && now - lastStutterDump >= std::chrono::seconds(30))
            {
                ++stutterDumps;
                lastStutterDump = now;
                std::fprintf(stderr, "[watchdog] stutter: %u hitches in under 3 s; sampling guest stacks\n",
                             hitches - hitchMark);
                LogStutterSample(stutterDumps, hitches - hitchMark);
                DumpGuestThreadStates();
                DumpGuestHotStacks();
                DumpFenceWaiters();
                DumpGuestAliasUsage();
                hitchMark = perf_stats::HitchCount();
                hitchMarkAt = std::chrono::steady_clock::now();
                std::fprintf(stderr, "[watchdog] stutter sample %d done\n", stutterDumps);
            }
        }
        // The dumps above take seconds (1.5 s of stack sampling): not a suspension. appletMainLoop below may
        // block while the game is in the background, and that time is one.
        suspend.last = SteadyNs();
        // HOME -> X: am asks the applet to exit. SDL's Switch driver turns that
        // into SDL_EVENT_QUIT - the clean shutdown that logged exit_code=0 - but
        // only while the main loop pumps events, which a hung game never does;
        // Horizon then kills it. Poll the applet on this thread too, so closing
        // the game stays a normal quit even when the renderer is stuck.
        const bool appletQuit = !appletMainLoop();
        // HOME -> X, whichever thread took the message: close for the system
        // (CloseForHomeMenu), not the quit to hbmenu.
        if (appletQuit || g_homeCloseRequested.load(std::memory_order_acquire))
        {
            std::fprintf(stderr, "[watchdog] HOME -> X: the system asked the game to close; releasing the exit lock\n");
            CloseForHomeMenu();
        }
    }
}
} // namespace

void StartExitWatchdog()
{
    if (g_exitWatchStarted)
        return;
    g_exitWatchStarted = true;
    // Above the game's threads (0x3B) so busy cores cannot starve it, below
    // SDL's audio and the log writer (0x2A).
    if (R_SUCCEEDED(threadCreate(&g_exitWatchThread, ExitWatchdogMain, nullptr, nullptr, 0x8000, 0x2B, -2)) &&
        R_SUCCEEDED(threadStart(&g_exitWatchThread)))
        return;
    g_exitWatchStarted = false;
    std::fprintf(stderr, "[watchdog] the exit watchdog thread could not be started\n");
}
#endif

void ShowError(const std::string& summary, const std::string& details)
{
    std::fprintf(stderr, "fatal: %s\n%s\n", summary.c_str(), details.c_str());
#if defined(__SWITCH2__)
    // Under kaiHEN's title takeover the system error applet cannot be shown: it
    // ends in "The software was closed because an error occurred", which unloads
    // the exploit. Leave the way a normal quit does instead - ExitNow flushes
    // stderr and the runtime log, asks the applet manager to exit and ends the
    // process - so the console returns to hbmenu with the session log on the SD
    // card. The fatal line above and the runtime log are the report.
    ExitNow();
#else
    ErrorApplicationConfig config{};
    if (R_SUCCEEDED(errorApplicationCreate(&config, summary.c_str(), details.c_str())))
        errorApplicationShow(&config);
#endif
}

bool IsDocked()
{
    return appletGetOperationMode() == AppletOperationMode_Console;
}

bool SystemLanguageIsSpanish()
{
    if (R_FAILED(setInitialize()))
        return false;
    u64 code = 0;
    SetLanguage language = SetLanguage_ENUS;
    const bool spanish = R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &language)) &&
                         (language == SetLanguage_ES || language == SetLanguage_ES419);
    setExit();
    return spanish;
}

bool TakeOperationModeChanged()
{
    return g_operationModeChanged.exchange(false, std::memory_order_acq_rel);
}

static std::atomic<std::string (*)()> g_memorySummaryExtra{nullptr};

void SetMemorySummaryExtra(std::string (*extra)())
{
    g_memorySummaryExtra.store(extra, std::memory_order_release);
}

std::string MemorySummary()
{
    u64 used = 0, total = 0;
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    char threads[48] = "";
#if !defined(__SWITCH2__)
    // kaiHEN's donor NPDM masks the resource-limit syscalls (svcGetInfo with
    // InfoType_ResourceLimit and svcGetResourceLimit*), and a masked svc* ends
    // the process instead of returning an error. This summary is the first thing
    // that runs after "thread placement: the process may use cores ...", the
    // last line of the 22:22:52 boot: one masked call there kills the boot with
    // no exception to report. Switch 2 keeps the thread counts out.
    u64 threadsUsed = 0, threadsLimit = 0;
    Handle limit = INVALID_HANDLE;
    if (R_SUCCEEDED(svcGetInfo(reinterpret_cast<u64*>(&limit), InfoType_ResourceLimit, INVALID_HANDLE, 0)) && limit != INVALID_HANDLE)
    {
        s64 current = 0, maximum = 0;
        if (R_SUCCEEDED(svcGetResourceLimitCurrentValue(&current, limit, LimitableResource_Threads)))
            threadsUsed = u64(current);
        if (R_SUCCEEDED(svcGetResourceLimitLimitValue(&maximum, limit, LimitableResource_Threads)))
            threadsLimit = u64(maximum);
        svcCloseHandle(limit);
    }
    std::snprintf(threads, sizeof(threads), ", threads %llu/%llu",
        static_cast<unsigned long long>(threadsUsed), static_cast<unsigned long long>(threadsLimit));
#endif
    // The heap is where nearly everything lives (guest memory chunks, NVK
    // buffers, thread stacks); UsedMemorySize does not count it for us.
    const struct mallinfo heap = mallinfo();
    // libnx gives newlib's heap [fake_heap_start, fake_heap_end); the arena can grow up to that.
    const size_t heapLimit = size_t(fake_heap_end - fake_heap_start);
    char text[224];
    std::snprintf(text, sizeof(text), "heap in use %llu MiB (arena %llu of %llu MiB), process %llu/%llu MiB%s",
        static_cast<unsigned long long>(size_t(heap.uordblks) >> 20), static_cast<unsigned long long>(size_t(heap.arena) >> 20),
        static_cast<unsigned long long>(heapLimit >> 20),
        static_cast<unsigned long long>(used >> 20), static_cast<unsigned long long>(total >> 20), threads);
    if (const auto extra = g_memorySummaryExtra.load(std::memory_order_acquire))
        return text + extra();
    return text;
}

void HeapUsage(uint64_t& used, uint64_t& limit)
{
    const struct mallinfo heap = mallinfo();
    used = uint64_t(heap.uordblks);
    limit = uint64_t(fake_heap_end - fake_heap_start);
}

bool ReadClockRates(uint32_t& cpuHz, uint32_t& gpuHz, uint32_t& memoryHz)
{
    cpuHz = gpuHz = memoryHz = 0;
    if (R_FAILED(clkrstInitialize()))
        return false;
    bool ok = true;
    const auto read = [&ok](PcvModuleId module, uint32_t& hz) {
        ClkrstSession session{};
        u32 rate = 0;
        if (R_SUCCEEDED(clkrstOpenSession(&session, module, 3)))
        {
            ok &= R_SUCCEEDED(clkrstGetClockRate(&session, &rate));
            clkrstCloseSession(&session);
        }
        else
            ok = false;
        hz = rate;
    };
    read(PcvModuleId_CpuBus, cpuHz);
    read(PcvModuleId_GPU, gpuHz);
    read(PcvModuleId_EMC, memoryHz);
    clkrstExit();
    return ok;
}

uint32_t CurrentThreadHandle()
{
    return threadGetCurHandle();
}

static std::atomic<uint32_t> g_pinnedThread{0}; // the game's own affinity calls leave this one alone

uint32_t SetThreadIdealCore(uint32_t handle, int core)
{
    if ((handle ? handle : threadGetCurHandle()) == g_pinnedThread.load(std::memory_order_relaxed))
        return 0;
    return svcSetThreadCoreMask(handle ? Handle(handle) : CUR_THREAD_HANDLE, core, 0x7);
}

void SleepNanoseconds(int64_t nanoseconds)
{
    svcSleepThread(nanoseconds);
}

void PinCurrentThread(int core)
{
    if (R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core)))
        g_pinnedThread.store(threadGetCurHandle(), std::memory_order_relaxed);
}

uint64_t ProcessCoreMask()
{
    u64 mask = 0;
    return R_SUCCEEDED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) ? mask : 0;
}

bool RestrictCurrentThreadToCores(uint64_t mask)
{
    if (!mask) return false;
    const s32 ideal = s32(__builtin_ctzll(mask));
    return R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, ideal, u32(mask)));
}

void SetCurrentThreadPriority(int priority)
{
    SetThreadPriority(0, priority);
}

int CurrentThreadPriority()
{
    s32 priority = -1;
    return R_SUCCEEDED(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE)) ? priority : -1;
}

uint32_t SetThreadPriority(uint32_t handle, int priority)
{
    return svcSetThreadPriority(handle ? Handle(handle) : CUR_THREAD_HANDLE, u32(priority));
}

void SetLoadingBoost(bool enabled)
{
    appletSetCpuBoostMode(enabled ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}
}
#endif

#if defined(__SWITCH2__)
// ---- Ending the runtime's host threads at exit (see RunExit) -----------------
// Linked in with -Wl,--wrap=__libc_cond_wait,--wrap=__libc_cond_wait_recursive,
// --wrap=svcSleepThread (LostOdysseyRecomp/CMakeLists.txt). Every
// std::condition_variable and pthread_cond wait ends in __libc_cond_wait, and
// every sleep (std::this_thread::sleep_for, nanosleep) in svcSleepThread. Until
// the exit begins each costs one relaxed-order load more; a wait with no
// timeout also takes a slot in g_condWaits so that RunExit can wake it.
extern "C"
{
int __real___libc_cond_wait(_COND_T* cond, _LOCK_T* lock, uint64_t timeout_ns);
int __real___libc_cond_wait_recursive(_COND_T* cond, _LOCK_RECURSIVE_T* lock, uint64_t timeout_ns);
void __real_svcSleepThread(s64 nano);

int __wrap___libc_cond_wait(_COND_T* cond, _LOCK_T* lock, uint64_t timeout_ns)
{
    if (ShouldEndThisThread())
    {
        __libc_lock_release(lock);
        os::switch_platform::EndThreadForExit();
    }
    const int slot = timeout_ns == UINT64_MAX ? RegisterCondWait(cond) : -1;
    const int result = __real___libc_cond_wait(cond, lock, timeout_ns);
    UnregisterCondWait(slot);
    if (ShouldEndThisThread())
    {
        __libc_lock_release(lock); // the wait took it again
        os::switch_platform::EndThreadForExit();
    }
    return result;
}

int __wrap___libc_cond_wait_recursive(_COND_T* cond, _LOCK_RECURSIVE_T* lock, uint64_t timeout_ns)
{
    if (ShouldEndThisThread())
    {
        __libc_lock_release_recursive(lock);
        os::switch_platform::EndThreadForExit();
    }
    const int slot = timeout_ns == UINT64_MAX ? RegisterCondWait(cond) : -1;
    const int result = __real___libc_cond_wait_recursive(cond, lock, timeout_ns);
    UnregisterCondWait(slot);
    if (ShouldEndThisThread())
    {
        __libc_lock_release_recursive(lock);
        os::switch_platform::EndThreadForExit();
    }
    return result;
}

void __wrap_svcSleepThread(s64 nano)
{
    if (ShouldEndThisThread())
        os::switch_platform::EndThreadForExit();
    __real_svcSleepThread(nano);
    if (ShouldEndThisThread())
        os::switch_platform::EndThreadForExit();
}
}
#endif

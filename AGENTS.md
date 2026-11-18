# LostOdysseyRecomp Switch 2 Port - Current State & Handoff

## Project & Build Environment
- **Repo Root:** `~/switch2-dev/LostOdysseyRecomp`
- **Build & Deploy Command:**
  `export DEVKITPRO=/opt/devkitpro && . /opt/devkitpro/switch2vars.sh && cmake --build build -j20 && cp -f build/LostOdysseyRecomp/LostOdysseyRecomp.nro /mnt/c/Users/Jorda/Desktop/LostOdysseyRecomp.nro`

## Completed Fixes (Already Committed in Git)
Read `summary/session_summary.txt`, `summary/summary_002.txt`, and `summary/summary_003.txt` for full technical context:
1. **`gpu/command_processor.cpp`:** Fixed the `WAIT_REG_MEM` `0x10002` loading-screen stall caused by priority inversion on Horizon OS (`InterruptMain`/`VsyncMain` elevated to priority `0x2B`, `kMinimumPollMs = 1` on `LO_PLATFORM_SWITCH`).
2. **`apu/audio.cpp` & `debug/perf_stats.cpp`:** Added `atexit` stop handlers and `ShuttingDown()` guards for `td214` and `t25b5`.
3. **`os/log_collection.cpp`:** Added shutdown guards for the detached uploader.
4. **kaiHEN R -> hbmenu hook (commit 5cde9314, confirmed working on hardware 2026-11-18):** never send `SetCurrentProcess` (or any identity/registration request) on a session obtained through `smGetService` on Switch 2. Those are libounce overrides shared with kaiHEN's HEN (see summary_016). `RawInitialize` in `os/switch/switch_runtime.cpp` must keep using `fsOpenSdCardFileSystem`. Do not regress this.

## Remaining Tasks to Finish
1. **`os/logger.cpp`:** Add a global `g_logClosed` latch so that once exit begins, all `LOG_*` calls become immediate no-ops after one final flush (prevents Switch OS error dialog during `fsdevUnmountAll()`).
2. **`modding/text_overlay.cpp` & Quit Path:** Ensure `StopPrebuild()` performs a bounded wait (`WaitForPrebuild()`) and skips `fs::remove_all` / cache writes on exit, and ensure `StopMonitor()` / `StopDriverThread()` / `g_logClosed` run even if `ExitNow()` or `FinishRequestedExit()` skips `std::atexit`.
3. **Summary Rule:** After every turn where you modify files or diagnose logs, write a new numbered summary file (`summary/summary_004.txt`, `summary/summary_005.txt`, etc.).

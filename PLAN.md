# Switch 2 (kaiHEN + OunceVK) port plan

Status: plan only. No files have been edited yet.

## Context: what the existing build does

### Switch 1 (`cmake/LoSwitch.cmake`, `thirdparty/CMakeLists.txt`, `LostOdysseyRecomp/CMakeLists.txt`)
- `LO_TARGET_PLATFORM=switch` comes from `LO_TARGET_SWITCH`, set by
  `cmake/toolchains/switch-devkitA64.cmake` (Cortex-A57, libnx,
  `-specs=libnx/switch.specs`).
- Vulkan is **already** the default via plume: `thirdparty/CMakeLists.txt` adds
  plume, and for switch defines `__SWITCH__` + `VK_USE_PLATFORM_VI_NN` and links
  `LO_SWITCH_NVK_LIBRARY` (`portlibs/switch/lib/libvulkan.a`, Mesa NVK) with the
  ICD bootstrap `-Wl,-u,vk_icdGetInstanceProcAddr ... --start-group`.
- deko3d is **additive**, not an alternative configure: `LO_SWITCH_BACKEND=deko3d`
  in `env.txt` (`gpu/video.cpp`, `IsDeko3d()`), compiled in whenever
  `TARGET lo_plume_deko3d`, which defines `LO_SWITCH_DEKO3D=1` and links
  `thirdparty/deko3d` + `uam` + `SPIRV-Cross` + `thirdparty/plume-deko3d`.
- Switch-specific plumbing: `--wrap=nwindowQueueBuffer/nwindowDequeueBuffer`,
  `--wrap=rename/getenv`, `os/switch/*` with `SKIP_PRECOMPILE_HEADERS`,
  `LO_SWITCH_DATA_ROOT`, `LO_SWITCH_DXC_IDENTITY`, NRO packaging via
  `elf2nro`/`nacptool`.
- PC/Android instead take `PLUME_SDL_VULKAN_ENABLED` (SDL surface) or Android's
  forced `PLUME_SDL_VULKAN_ENABLED`; nothing to port from there except the
  *idea* that Vulkan is the only backend.

### Switch 2 environment (`/opt/devkitpro/libounce`, examples)
- `switch2vars.sh` sets `PORTLIBS_SWITCH2=.../portlibs/switch2`, `-D__SWITCH2__`,
  `-mcpu=cortex-a78c+crypto`, `LIBS=-lounce`.
- `aarch64-none-elf-cmake` is a wrapper for
  `${DEVKITPRO}/libounce/cmake/Switch2.cmake`; it keeps
  `CMAKE_SYSTEM_NAME=NintendoSwitch`, sets `NINTENDO_SWITCH` **and**
  `NINTENDO_SWITCH2`, replaces `NX_ROOT` with libounce (`-lnx` -> libounce), and
  puts switch2 portlibs first.
- `portlibs/switch2/lib/libvulkan.a` is just `INPUT(-louncevk -lstdc++ -lm)`;
  `libouncevk.a` exports `vkCreateInstance`,
  `vkEnumerateInstanceExtensionProperties`, `vkCreateViSurfaceNN` **directly**,
  so no ICD-loader bootstrap is needed.
- `examples/vulkan-clear` creates the surface exactly like Switch 1:
  `VK_NN_VI_SURFACE_EXTENSION_NAME` +
  `vkCreateViSurfaceNN(instance, {.window = nwindowGetDefault()}, ...)`, then a
  normal KHR swapchain with `caps.currentExtent` (so 1920x1080 comes from the
  surface, not a hardcoded size). `LIBS := -lvulkan -lnx`.
- There is **no** `libdeko3d`, `libnx`, SDL or NVK in the switch2 tree;
  `libnx.a`/`libnxd.a` in `libounce/lib` are 18-byte `INPUT()` stubs.

### Two decisions to settle before editing
1. **Runtime shader compilation.** `CLAUDE.md` says to enable shaderc/glslang,
   but this repo has neither; its Vulkan path compiles SPIR-V at runtime with the
   pinned x86 **DXC** (`gpu/shader/dxc_compiler.cpp`), which cannot run on a
   console. Switch builds deliberately ship the prebuilt `portable_vk.lospv` pack
   + PC cache. This plan keeps that (it is the only working console path) rather
   than adding a new shader compiler.
2. **Platform identity.** Keep `LO_TARGET_PLATFORM=switch` and add a
   `LO_TARGET_SWITCH2` flag, so the `#if LO_PLATFORM_SWITCH` plumbing (paths,
   logs, hid, audren, LB+RB menu) survives unchanged and only the GPU/toolchain
   branches differ. A separate `switch2` platform id would be cleaner but touches
   dozens of call sites for no functional gain.

## Plan

1. **Toolchain / platform detection**
   - Add `cmake/toolchains/switch2-libounce.cmake` (or a preset) that includes
     `${DEVKITPRO}/libounce/cmake/Switch2.cmake` and then sets
     `LO_TARGET_SWITCH=ON` + `LO_TARGET_SWITCH2=ON`, so
     `aarch64-none-elf-cmake` users and a repo toolchain behave identically.
   - In `cmake/LoPlatform.cmake`, derive `LO_TARGET_SWITCH2` from
     `NINTENDO_SWITCH2` (default OFF), keep `LO_TARGET_PLATFORM=switch`; print it
     in the status line.

2. **Disable deko3d for Switch 2**
   - `thirdparty/CMakeLists.txt`: wrap the whole
     deko3d/`uam`/`SPIRV-Cross`/`lo_plume_deko3d` block and the `lo_lsfg` block
     in `AND NOT LO_TARGET_SWITCH2` (neither has a Switch 2 driver); keep plume.
   - `LostOdysseyRecomp/CMakeLists.txt`: skip the `lo_plume_deko3d` link, the
     `LO_SWITCH_DEKO3D=1` define, `lo_switch_pgo_target(lo_plume_deko3d)` and
     `--wrap=nwindowQueueBuffer,--wrap=nwindowDequeueBuffer` when
     `LO_TARGET_SWITCH2`.
   - `cmake/LoSwitchPgo.cmake` / `LoSwitch.cmake`: accept SPIR-V shader-cache
     options as-is; no `.dksh` cache rules exist in CMake beyond `uam`'s test
     target, so nothing else to strip.

3. **Switch the Vulkan driver to OunceVK**
   - `cmake/LoSwitch.cmake`: default `LO_SWITCH_NVK_LIBRARY` ->
     `${DEVKITPRO}/portlibs/switch2/lib/libvulkan.a` when `LO_TARGET_SWITCH2`, and
     drop `LO_SWITCH_NVK_EXTRA_LIBS` (no drm_nouveau/expat/zstd on switch2).
   - `thirdparty/CMakeLists.txt` switch branch: link plain `vulkan`
     (i.e. `-lvulkan` -> `libouncevk`) instead of `--start-group NVK` +
     `-Wl,-u,vk_icdGetInstanceProcAddr`; keep `nx` (= libounce) and
     `-D__SWITCH__ VK_USE_PLATFORM_VI_NN`.

4. **Surface / swapchain / 1080p60**
   - Confirm `thirdparty/plume`'s switch path (patched for `VK_NN_vi_surface` +
     `nwindowGetDefault()`) compiles against the switch2 `vulkan/vulkan.h` and
     needs no code change; only fix it if the header differs.
   - Set the Switch 2 output/render target to 1920x1080 (docked default) in
     `settings/config.cpp` via the existing `SwitchMaximumHeight()` /
     `internalResolution` path rather than hardcoding an extent, so fullscreen
     and scaling still work.
   - Remove the 30 FPS pin: `settings/config.cpp`
     `value.frameRate = gpu::frame_rate::kDefault` and
     `settings/graphics_menu.h` `ApplyPreset` `config.frameRate = 30`, allowing
     60 on switch2 (keep 30 as the default cap for Switch 1). Verify the
     `LO_FPS` override and the `frame_plan.cpp` budget still behave.

5. **Preserve Switch OS plumbing**
   - Keep `LO_SWITCH_DATA_ROOT=/switch/LostOdysseyRecomp`,
     `LO_SWITCH_DXC_IDENTITY`, `os/switch/*` sources and their
     `SKIP_PRECOMPILE_HEADERS`, the `rename`/`getenv` wraps, and `-lnx`.
   - Re-check `os/switch/switch_runtime.cpp` / `nvk_switch_stubs.c` includes
     against libounce (`switch.h`, `switch2/services/*`); compile errors there
     are expected to be the only source-level changes.

6. **Build & verify (per phase, per `CLAUDE.md`)**
   - `source /opt/devkitpro/switch2vars.sh && aarch64-none-elf-cmake -S . -B
     build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)`.
   - Gate: configure log shows `LostOdysseyRecomp target: switch-aarch64`, no
     `deko3d back end` line, and `libvulkan.a` resolving to `portlibs/switch2`.
   - Compile-first iteration; link check only after the runtime sources build.
     Note that `thirdparty/SDL-switch` is libnx-based and is the most likely ABI
     risk against libounce; any failure there is a blocker to report, not
     something to patch around.

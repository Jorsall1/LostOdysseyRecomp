# cmake/LoSwitch.cmake
# Nintendo Switch (Horizon homebrew) build settings. Included from the top-level
# CMakeLists.txt when LO_TARGET_PLATFORM is "switch". See docs/SWITCH.md.

# PC-only features. None of them has a Switch implementation: the renderer is
# Vulkan on Mesa NVK (Switch 1) or OunceVK (Switch 2), there is no updater or
# network shader download, and shaders come from a prebuilt SPIR-V pack (no DXC
# on the console).
foreach(_lo_switch_off IN ITEMS
        LO_ENABLE_DLSS LO_ENABLE_FSR LO_ENABLE_STREAMLINE_FG LO_ENABLE_XESS LO_ENABLE_XESS_FG
        LO_ENABLE_D3D12_DLSS_FG LO_ENABLE_FSR_FG LO_ENABLE_VULKAN_FSR_FG LO_ENABLE_METALFX_FG)
    set(${_lo_switch_off} OFF CACHE BOOL "Not available on Nintendo Switch" FORCE)
endforeach()

set(DEVKITPRO "/opt/devkitpro" CACHE PATH "devkitPro root")

# Switch 2 packaging tools: libounce's toolchain finds switch-tools under its
# own NX_*_EXE names, while the runtime target uses the LO_SWITCH_* entries.
if(LO_TARGET_SWITCH2)
    if(NOT LO_SWITCH_ELF2NRO)
        if(NX_ELF2NRO_EXE)
            set(LO_SWITCH_ELF2NRO "${NX_ELF2NRO_EXE}" CACHE FILEPATH "elf2nro (switch-tools)")
        else()
            find_program(LO_SWITCH_ELF2NRO elf2nro HINTS "${DEVKITPRO}/tools/bin")
        endif()
    endif()
    if(NOT LO_SWITCH_NACPTOOL)
        if(NX_NACPTOOL_EXE)
            set(LO_SWITCH_NACPTOOL "${NX_NACPTOOL_EXE}" CACHE FILEPATH "nacptool (switch-tools)")
        else()
            find_program(LO_SWITCH_NACPTOOL nacptool HINTS "${DEVKITPRO}/tools/bin")
        endif()
    endif()
endif()

if(LO_TARGET_SWITCH2)
    # OunceVK: libounce's Vulkan implementation, linked statically. Its
    # libvulkan.a is an INPUT() stub for libouncevk.a and exports
    # vkCreateInstance/vkCreateViSurfaceNN directly, so there is no ICD loader
    # to bootstrap. It has no dependency on drm_nouveau/expat/zstd.
    set(LO_SWITCH_NVK_LIBRARY "${DEVKITPRO}/portlibs/switch2/lib/libvulkan.a"
        CACHE FILEPATH "OunceVK Switch 2 static Vulkan driver (libvulkan.a)")
    set(LO_SWITCH_NVK_EXTRA_LIBS "")
    if(NOT LO_SWITCH_CHECK_ONLY)
        message(STATUS "LostOdysseyRecomp: OunceVK ${LO_SWITCH_NVK_LIBRARY}")
    endif()
else()
    # Mesa NVK for Horizon, linked statically (no Vulkan loader on the console).
    # The switch-dev Docker image installs it into devkitPro's portlibs.
    set(LO_SWITCH_NVK_LIBRARY "${DEVKITPRO}/portlibs/switch/lib/libvulkan.a"
        CACHE FILEPATH "Mesa NVK Switch static Vulkan driver (libvulkan.a)")

    # Libraries the NVK archive may reference, depending on how mesa-switch was
    # built (UnleashedRecomp-NX links expat and drm_nouveau only when present).
    set(LO_SWITCH_NVK_EXTRA_LIBS "")
    if(NOT LO_SWITCH_CHECK_ONLY)
        foreach(_lo_nvk_dep IN ITEMS drm_nouveau expat zstd z)
            find_library(LO_SWITCH_LIB_${_lo_nvk_dep} ${_lo_nvk_dep}
                HINTS "${DEVKITPRO}/portlibs/switch/lib" "${DEVKITPRO}/libnx/lib")
            if(LO_SWITCH_LIB_${_lo_nvk_dep})
                list(APPEND LO_SWITCH_NVK_EXTRA_LIBS "${LO_SWITCH_LIB_${_lo_nvk_dep}}")
            endif()
        endforeach()
        message(STATUS "LostOdysseyRecomp: NVK ${LO_SWITCH_NVK_LIBRARY} (+ ${LO_SWITCH_NVK_EXTRA_LIBS})")
    endif()
endif()

# Switch releases (LostOdysseyNX, tags vX.Y.Z_NX) are numbered apart from the PC
# project version (project() in CMakeLists.txt, which follows upstream). It is
# the NRO version below and the source version in the logs and crash.log.
set(LO_SWITCH_RELEASE_VERSION "1.2.0")
set(LO_SOURCE_VERSION "${LO_SWITCH_RELEASE_VERSION}")
if(LO_VERSION_SUFFIX)
    string(APPEND LO_SOURCE_VERSION "-${LO_VERSION_SUFFIX}")
endif()

# NRO metadata shown in the Homebrew Menu.
set(LO_SWITCH_APP_TITLE "Lost Odyssey Recomp" CACHE STRING "NRO title")
set(LO_SWITCH_APP_AUTHOR "LostOdysseyRecomp contributors" CACHE STRING "NRO author")
# The NRO version follows the project version (CMakeLists.txt project()) on
# every configure. It used to be a cache entry, which kept the value of the
# first configure (0.9.0) after main moved on. LO_SWITCH_APP_VERSION_OVERRIDE
# replaces it when set.
set(LO_SWITCH_APP_VERSION_OVERRIDE "" CACHE STRING "NRO version (empty: the project version)")
unset(LO_SWITCH_APP_VERSION CACHE)
if(LO_SWITCH_APP_VERSION_OVERRIDE)
    set(LO_SWITCH_APP_VERSION "${LO_SWITCH_APP_VERSION_OVERRIDE}")
else()
    set(LO_SWITCH_APP_VERSION "${LO_SOURCE_VERSION}")
endif()
set(LO_SWITCH_APP_ICON "${CMAKE_SOURCE_DIR}/packaging/switch/icon.jpg" CACHE FILEPATH "256x256 JPEG NRO icon")

# Game data and user files on the SD card. The game folder holds disc1..disc4
# exactly as the PC importer writes them (default.xex, LO.fpi, *.fpd).
# "/switch/..." rather than "sdmc:/switch/...": libnx maps absolute paths to the
# SD card, while std::filesystem treats "sdmc:" as a relative folder name.
set(LO_SWITCH_DATA_ROOT "/switch/LostOdysseyRecomp" CACHE STRING "SD card folder for game data, settings and logs" FORCE)

# Compiler identity the console reports for shader caches made on a PC: the
# pinned DXC of tools/XenosRecomp/thirdparty/dxc-bin ("dxc-<major>.<minor>").
set(LO_SWITCH_DXC_IDENTITY "dxc-1.8" CACHE STRING "DXC identity of PC-made Vulkan shader caches")

# Code generation options (same experiments as UnleashedRecomp-NX).
option(LO_SWITCH_RECOMP_O2 "Compile the recompiled game code with -O2 instead of -O3" OFF)
option(LO_SWITCH_FP_CONTRACT_OFF "Compile the recompiled game code with -ffp-contract=on instead of fast (A/B tests, #4)" OFF)
# Diagnostics build (os/switch/heap_census.cpp): newlib's allocator goes through the census wrappers, which
# LO_HEAP_CENSUS=1 in env.txt starts. Off: the default build has neither the wrappers nor the census code.
option(LO_SWITCH_HEAP_CENSUS "Link the heap census (allocation counts by size and call site, LO_HEAP_CENSUS)" OFF)
option(LO_SWITCH_LTO "Link-time optimisation (needs a lot of RAM on the build machine)" OFF)
set(LO_SWITCH_LTO_JOBS "2" CACHE STRING "Parallel LTRANS jobs for the LTO link")
if(LO_SWITCH_LTO)
    add_compile_options(-flto -fno-fat-lto-objects)
    add_link_options(-flto=${LO_SWITCH_LTO_JOBS} -flto-partition=balanced)
endif()
# Link-time optimisation of the per-draw code only (LostOdysseyRecomp/CMakeLists.txt): the renderer, the command
# processor and plume-deko3d, whose calls into each other are the GPU CmdProc thread's draw path.
option(LO_SWITCH_LTO_GPU "Link-time optimisation of the renderer, command processor and plume-deko3d" OFF)

include(cmake/LoSwitchPgo.cmake)

# Development check build (tools/switch/check-toolchain.cmake): compiles with a
# stock aarch64 Linux GCC and the libnx headers, links nothing.
if(LO_SWITCH_CHECK_ONLY)
    message(STATUS "LostOdysseyRecomp: Switch compile check only (no NRO)")
endif()

# Nintendo Switch 2 (kaiHEN) homebrew cross toolchain: devkitPro devkitA64 +
# libounce (https://github.com/.../libounce), GPU through OunceVK.
#
#   source /opt/devkitpro/switch2vars.sh
#   cmake -S . -B build -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/switch2-libounce.cmake \
#     -DCMAKE_BUILD_TYPE=Release
#
# libounce also installs a wrapper, aarch64-none-elf-cmake, that selects its own
# Switch2.cmake; both routes configure the same build, because Switch2.cmake
# sets NINTENDO_SWITCH2 and cmake/LoPlatform.cmake derives LO_TARGET_SWITCH2
# from it.
#
# The Switch 2 keeps LO_TARGET_PLATFORM=switch: paths, OS services, HID, audio
# and the menu plumbing are shared with Switch 1. LO_TARGET_SWITCH2 selects the
# toolchain ABI and the OunceVK driver instead of libnx + Mesa NVK + deko3d.

if(NOT DEFINED DEVKITPRO)
    if(DEFINED ENV{DEVKITPRO})
        set(DEVKITPRO "$ENV{DEVKITPRO}")
    else()
        set(DEVKITPRO "/opt/devkitpro")
    endif()
endif()

set(_LO_SWITCH2_TOOLCHAIN "${DEVKITPRO}/libounce/cmake/Switch2.cmake")
if(NOT EXISTS "${_LO_SWITCH2_TOOLCHAIN}")
    message(FATAL_ERROR
        "libounce's CMake toolchain not found: ${_LO_SWITCH2_TOOLCHAIN} "
        "(install the Switch 2 devkit or set DEVKITPRO)")
endif()
include("${_LO_SWITCH2_TOOLCHAIN}")

# LoPlatform.cmake keys LO_TARGET_PLATFORM=switch off LO_TARGET_SWITCH, as
# devkitPro's own toolchain does.
set(LO_TARGET_SWITCH ON CACHE BOOL "Build for Nintendo Switch (libnx/libounce)" FORCE)
set(LO_TARGET_SWITCH2 ON CACHE BOOL "Build for Nintendo Switch 2 (libounce/OunceVK)" FORCE)

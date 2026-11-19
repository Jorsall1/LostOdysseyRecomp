#pragma once
// Which of SDL's Switch pads have a controller (os/switch/npad_switch.cpp). SDL's Switch joystick driver
// (thirdparty/SDL-switch/src/joystick/switch/SDL_sysjoystick.c) always adds 8 pads, instance i + 1 for npad
// No(i + 1), instance 1 also covering the handheld npad, and reports all 8 connected; hid.cpp read every one
// of them each pump. Bit i of the mask: instance i + 1 has a controller (a non-empty npad style set). Reads
// HID shared memory only, no service request and no lock.
#include <cstdint>
namespace hid::switch_npad
{
    constexpr uint32_t kPadCount = 8;
    uint32_t ConnectedMask();
}

// hid/switch_npad.h. Here, not in hid.cpp: libnx's <switch.h> Mutex collides with the runtime's.
#include <os/platform.h>
#if LO_PLATFORM_SWITCH
#include <switch.h>
#include <hid/switch_npad.h>

uint32_t hid::switch_npad::ConnectedMask()
{
    uint32_t mask = 0;
    // SDL's pad 1 is padInitializeDefault: No1 and the handheld npad.
    if (hidGetNpadStyleSet(HidNpadIdType_No1) != 0 || hidGetNpadStyleSet(HidNpadIdType_Handheld) != 0) mask |= 1u;
    for (uint32_t i = 1; i < kPadCount; ++i)
        if (hidGetNpadStyleSet(HidNpadIdType(HidNpadIdType_No1 + i)) != 0) mask |= 1u << i;
    return mask;
}
#endif

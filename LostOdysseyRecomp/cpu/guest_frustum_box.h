#pragma once

#include <cstdint>

struct PPCContext;

// sub_823BDA78 (FConvexVolume::IntersectBox) as cpu/guest_frustum_box.cpp
// implements it: the native NEON test, or the recompiled code when
// LO_NATIVE_FRUSTUM_BOX=0 (2 runs both and compares). The function is not a
// PPC hook itself because gpu/hor_plus.cpp owns that symbol for the culling
// margin and calls this one, so both features apply to every call.
//
// Returns true when ctx.r3 holds the result, false when the caller should run
// the recompiled code (__imp__sub_823BDA78) itself.
bool RunGuestFrustumBox(PPCContext& ctx, uint8_t* base);

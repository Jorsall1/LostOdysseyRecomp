#include <stdafx.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <vector>
#include <kernel/memory.h>
#include <os/logger.h>
#include "settings/config.h"
#include "gpu/effect_budget.h"
#include "gpu/shadow_low.h"

// Dynamic shadows Low: per-object shadows only for objects on screen, and for
// at most the 12 largest of them. Each shadow costs a depth pass, its
// projection passes and their draws on the GPU and the render threads. In
// Uhra's main street the game makes about 60 a frame, 52 of them for people
// and objects that are not on screen; at 10 W they take a third of the GPU
// time.
//
// sub_823BE570 (InitDynamicShadows) runs once per frame and calls sub_823BE9D0
// for each light-primitive interaction (r3 the scene renderer, r4 the
// interaction). sub_823BE9D0 sizes the shadow from the primitive's screen size,
// the largest over the views of
//   max(view+0x24 * view+0x80, view+0x28 * view+0x94) * radius / max(depth, 1)
// (about the object's height in 720p pixels), then creates it unless the
// light's fade is zero; when it creates nothing it changes nothing. Low skips
// the interaction before that. Which casters keep a shadow: gpu/shadow_low.h
// (turn 81: 12 largest, hysteresis and a 0.5 s dwell, so shadows stop
// switching on and off as the player walks).
extern "C" PPC_FUNC(__imp__sub_823BE570);
extern "C" PPC_FUNC(__imp__sub_823BE9D0);

namespace
{
// Render thread only. Selection rules: gpu/shadow_low.h.
bool low = false;
gpu::shadow_low::Selector selector;
// Keyed by primitive scene info. A primitive lit by two lights has two
// interactions; both see the same size, so they decide alike.
std::unordered_map<uint32_t, gpu::shadow_low::Caster> casters;
uint32_t LoadU32(uint32_t address)
{
    return __builtin_bswap32(*static_cast<const uint32_t*>(g_memory.Translate(address)));
}
float LoadF32(uint32_t address)
{
    return std::bit_cast<float>(LoadU32(address));
}

// The primitive's screen size as sub_823BE9D0 computes it; its bounds include
// the attached primitives (+0x8C, next +0x90), here as one enclosing sphere.
float ScreenSize(uint32_t renderer, uint32_t primitive)
{
    const float x = LoadF32(primitive + 0x28), y = LoadF32(primitive + 0x2C), z = LoadF32(primitive + 0x30);
    float radius = LoadF32(primitive + 0x40);
    for (uint32_t child = LoadU32(primitive + 0x8C), guard = 0; child && guard < 64; child = LoadU32(child + 0x90), ++guard)
    {
        const float dx = LoadF32(child + 0x28) - x, dy = LoadF32(child + 0x2C) - y, dz = LoadF32(child + 0x30) - z;
        radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz) + LoadF32(child + 0x40));
    }
    const uint32_t views = LoadU32(renderer + 0x30);
    const uint32_t count = std::min<uint32_t>(LoadU32(renderer + 0x34), 8);
    float size = 0.0f;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t view = views + i * 0x6F0;
        const float scale = std::max(LoadF32(view + 0x24) * LoadF32(view + 0x80), LoadF32(view + 0x28) * LoadF32(view + 0x94)) * 0.5f;
        const float depth = LoadF32(view + 0x11C) * y + LoadF32(view + 0x10C) * x + LoadF32(view + 0x12C) * z + LoadF32(view + 0x13C);
        size = std::max(size, scale * radius / std::max(depth, 1.0f) * 2.0f);
    }
    return std::isfinite(size) ? size : 0.0f;
}

// Whether the primitive or an attached one is visible in a view: the bit of its
// index (+0x20) in the view's visibility map (+0x25C), as sub_823BE9D0 tests
// before it adds the half-size second shadow.
bool OnScreen(uint32_t renderer, uint32_t primitive)
{
    const uint32_t views = LoadU32(renderer + 0x30);
    const uint32_t count = std::min<uint32_t>(LoadU32(renderer + 0x34), 8);
    auto inView = [&](uint32_t p) {
        const int32_t index = int32_t(LoadU32(p + 0x20));
        if (index < 0) return false;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t bits = LoadU32(views + i * 0x6F0 + 0x25C);
            if (bits && (LoadU32(bits + uint32_t(index >> 5) * 4) >> (index & 31) & 1)) return true;
        }
        return false;
    };
    if (inView(primitive)) return true;
    for (uint32_t child = LoadU32(primitive + 0x8C), guard = 0; child && guard < 64; child = LoadU32(child + 0x90), ++guard)
        if (inView(child)) return true;
    return false;
}
}

PPC_FUNC(sub_823BE570)
{
    // Low from the settings, or from the effect budget (gpu/effect_budget.h) when shadows are On.
    const uint32_t setting = settings::GetConfig().dynamicShadows;
    const bool lowNow = setting == 2 || (setting == 1 && gpu::effect_budget::ShadowsLow());
    if (lowNow != low)
        LOG_INFO("graphics effects: dynamic shadows {}", lowNow ? "low" : "full");
    low = lowNow;
    selector.BeginFrame();
    if (selector.statFrames >= 600)
    {
        if (low)
            LOG_INFO("dynamic shadows low: {:.1f} casters on screen, {:.1f} shadows per frame (last {} frames)",
                double(selector.statCasters) / double(selector.statFrames),
                double(selector.statShown) / double(selector.statFrames), selector.statFrames);
        selector.statFrames = selector.statCasters = selector.statShown = 0;
    }
    if (!low || casters.size() > 4096)
        casters.clear();
    __imp__sub_823BE570(ctx, base);
}

PPC_FUNC(sub_823BE9D0)
{
    const uint32_t interaction = ctx.r4.u32;
    const uint32_t primitive = LoadU32(interaction + 8);
    // The same precondition as the original: only these interactions get a shadow.
    if (!low || (LoadU32(interaction + 0xC) & 0x18000000) != 0x10000000 || LoadU32(primitive + 0x88) != 0)
    {
        __imp__sub_823BE9D0(ctx, base);
        return;
    }
    gpu::shadow_low::Caster& caster = casters[primitive];
    if (!selector.Visible(caster, OnScreen(ctx.r3.u32, primitive)))
        return;
    if (selector.Decide(caster, ScreenSize(ctx.r3.u32, primitive)))
        __imp__sub_823BE9D0(ctx, base);
}

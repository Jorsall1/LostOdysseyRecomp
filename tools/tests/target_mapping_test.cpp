#include <gpu/render_resolution.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {
int checks = 0;
void Require(bool value, const char* name) { ++checks; if (!value) { std::fprintf(stderr, "FAIL: %s\n", name); std::exit(1); } }
}

int main()
{
    using namespace gpu::resolution;
    const Size plan{3440, 1440};
    Require(TargetSizeForRole(TargetRole::Scene, 1280, 720, plan) == plan, "scene maps full plan");
    Require(TargetSizeForRole(TargetRole::Scene, 480, 480, plan) == plan, "scene bypasses square heuristic");
    Require(TargetSizeForRole(TargetRole::Fixed, 880, 896, plan) == Size{}, "fixed remains native");
    Require(TargetSizeForRole(TargetRole::Unknown, 1280, 736, plan) == Size{2560, 1440}, "unknown retains isotropic height scale");
    // The shadow map's 880/440-pitch colour family: scene scale unless the Switch 2 rule keeps it at guest size.
    Require(TargetSizeForRole(TargetRole::Unknown, 880, 896, plan, false) == Size{2560, 1440}, "unknown near-square retains isotropic height scale");
    for (const Size scene : {Size{1920, 1080}, Size{2560, 1440}, Size{1024, 576}}) {
        Require(TargetSizeForRole(TargetRole::Unknown, 880, 896, scene, true) == Size{}, "shadow atlas colour keeps guest size");
        Require(TargetSizeForRole(TargetRole::Unknown, 880, 864, scene, true) == Size{}, "unpadded shadow atlas colour keeps guest size");
        Require(TargetSizeForRole(TargetRole::Unknown, 440, 448, scene, true) == Size{}, "half shadow keeps guest size");
        Require(TargetSizeForRole(TargetRole::Unknown, 880, 448, scene, true) == Size{}, "shadow strip keeps guest size");
        Require(TargetSizeForRole(TargetRole::Unknown, 1280, 736, scene, true) == scene, "scene target still scales");
        Require(TargetSizeForRole(TargetRole::Unknown, 640, 384, scene, true) == scene, "half scene target still scales");
    }
    Require(TargetSizeForPlan(TargetRole::Unknown, 880, 896, plan, plan, 1, true) == Size{}, "plan routing keeps the rule");
#ifdef __SWITCH2__
    Require(kShadowFamilyUnscaled && TargetSizeForRole(TargetRole::Unknown, 880, 896, Size{1920, 1080}) == Size{},
        "Switch 2 default keeps the shadow family at guest size");
#else
    Require(!kShadowFamilyUnscaled, "other platforms keep the old scaling by default");
#endif
    const uint32_t requestedHeight = 352, cachedHeight = 640;
    const uint32_t effectiveHeight = std::max(requestedHeight, cachedHeight);
    Require(TargetSizeForRole(TargetRole::Unknown, 640, requestedHeight, plan) == Size{2560, 1440}, "requested nonsquare extent would remap");
    Require(TargetSizeForRole(TargetRole::Unknown, 640, effectiveHeight, plan) == Size{}, "cached square extent keeps its fixed mapping");
    std::printf("target mapping: %d checks passed\n", checks);
}

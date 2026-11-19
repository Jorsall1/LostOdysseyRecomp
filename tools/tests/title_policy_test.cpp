// settings/title_policy.h: Y: Settings on the title everywhere, the Back quit dialog and legend off on Switch 2.
#include <settings/title_policy.h>
#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main()
{
    namespace p = settings::title_policy;
    const p::Policy switch2{true, false}, pc{true, true};
    // Switch 2: idle menu arms Y and shows the legend; the quit dialog never sees idle.
    {
        const auto t = p::Decide(switch2, true, false);
        CHECK(!t.quitIdle && t.settingsIdle && t.hint);
        CHECK(!p::ShowQuitLegend(switch2));
        // UpdateTitleQuit can only report ownership when it saw idle; even a stray true must not block Y.
        CHECK(p::Decide(switch2, true, true).settingsIdle);
    }
    // Not idle: nothing armed.
    {
        const auto t = p::Decide(switch2, false, false);
        CHECK(!t.quitIdle && !t.settingsIdle && !t.hint);
    }
    // Elsewhere: both, and an open quit dialog owns the input and hides the legend.
    {
        const auto t = p::Decide(pc, true, false);
        CHECK(t.quitIdle && t.settingsIdle && t.hint && p::ShowQuitLegend(pc));
        const auto open = p::Decide(pc, true, true);
        CHECK(open.quitIdle && !open.settingsIdle && !open.hint);
    }
#ifdef __SWITCH2__
    CHECK(!p::Policy{}.quit && p::Policy{}.settings);
#else
    CHECK(p::Policy{}.quit && p::Policy{}.settings);
#endif
    if (failures) return EXIT_FAILURE;
    std::puts("title_policy_test: ok");
    return 0;
}

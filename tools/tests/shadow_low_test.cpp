// gpu/shadow_low.h (turn 81). Build: g++ -std=c++20 -I LostOdysseyRecomp tools/tests/shadow_low_test.cpp
#include <gpu/shadow_low.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
namespace sl = gpu::shadow_low;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
int main()
{
    {
        // The salon pot: 11 steady casters plus a pot and a person whose sizes cross as the player walks
        // (person 60 +- 15 px, pot 55 px). Old rule (8 largest, 0.8 keep): the pot's shadow went on and off.
        sl::Selector s;
        sl::Caster steady[11], pot, person;
        int potChanges = 0;
        bool potLast = false, potEver = false;
        for (int f = 0; f < 600; ++f) {
            s.BeginFrame();
            for (int i = 0; i < 11; ++i) {
                s.Visible(steady[i], true);
                s.Decide(steady[i], 80.0f + i * 5);
            }
            s.Visible(person, true);
            s.Decide(person, 60.0f + 15.0f * std::sin(f * 0.2f));
            s.Visible(pot, true);
            const bool shown = s.Decide(pot, 55.0f);
            if (f > 0 && shown != potLast) ++potChanges;
            potLast = shown;
            potEver |= shown;
        }
        CHECK(potEver);
        CHECK(potChanges <= 1);
    }
    {
        // A size oscillating across the enter line every frame changes at most once per DwellFrames.
        sl::Selector s;
        sl::Caster big[12], edge;
        int changes = 0;
        bool last = false;
        for (int f = 0; f < 300; ++f) {
            s.BeginFrame();
            for (auto& c : big) { s.Visible(c, true); s.Decide(c, 100.0f); }
            s.Visible(edge, true);
            const bool shown = s.Decide(edge, (f & 1) ? 110.0f : 40.0f);
            if (f > 0 && shown != last) ++changes;
            last = shown;
        }
        CHECK(changes <= 300 / int(sl::DwellFrames) + 1);
    }
    {
        // Two lights: the second interaction repeats the first decision and is not counted twice.
        sl::Selector s;
        sl::Caster c;
        s.BeginFrame();
        s.Visible(c, true);
        const bool a = s.Decide(c, 50.0f);
        s.Visible(c, true);
        const bool b = s.Decide(c, 50.0f);
        CHECK(a == b && s.sizes.size() == 1);
    }
    {
        // Off screen: shadow kept OffScreenFrames, then none; back on screen it decides at once.
        sl::Selector s;
        sl::Caster c;
        s.BeginFrame();
        s.Visible(c, true);
        CHECK(s.Decide(c, 50.0f));
        for (uint32_t f = 0; f < sl::OffScreenFrames; ++f) {
            s.BeginFrame();
            CHECK(s.Visible(c, false));
            CHECK(s.Decide(c, 50.0f));
        }
        s.BeginFrame();
        CHECK(!s.Visible(c, false));
        s.BeginFrame();
        CHECK(s.Visible(c, true));
        CHECK(s.Decide(c, 50.0f));
    }
    {
        // Tiny casters (under MinSize) get none; MaxCount to MaxCount + KeepSlack shown with many casters.
        sl::Selector s;
        sl::Caster tiny, many[40];
        uint32_t shown = 0;
        for (int f = 0; f < 120; ++f) {
            s.BeginFrame();
            s.Visible(tiny, true);
            CHECK(!s.Decide(tiny, 8.0f));
            shown = 0;
            for (int i = 0; i < 40; ++i) {
                s.Visible(many[i], true);
                shown += s.Decide(many[i], 20.0f + i * 3);
            }
        }
        CHECK(shown >= sl::MaxCount && shown <= sl::MaxCount + sl::KeepSlack);
    }
    if (failures) return EXIT_FAILURE;
    std::puts("shadow_low_test: ok");
    return 0;
}

// thirdparty/plume/plume_spirv_trim.h StripNoContraction / AddEarlyFragmentTests (turn 53).
// Build: g++ -std=c++20 -I thirdparty/plume tools/tests/spirv_lean_test.cpp
#include <plume_spirv_trim.h>
#include <cstdio>
namespace st = plume::spirv_trim;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
static uint32_t Op(uint32_t count, uint32_t opcode) { return (count << 16) | opcode; }
// header, OpCapability Shader, OpMemoryModel, OpEntryPoint <model> %1 "main", [mode], decorations, a body word.
static std::vector<uint32_t> Module(uint32_t model, bool earlyAlready)
{
    std::vector<uint32_t> w{0x07230203u, 0x00010000u, 0, 20, 0};
    w.insert(w.end(), {Op(2, 17), 1});                     // OpCapability Shader
    w.insert(w.end(), {Op(3, 14), 0, 1});                  // OpMemoryModel Logical GLSL450
    w.insert(w.end(), {Op(5, 15), model, 1, 0x6E69616D, 0}); // OpEntryPoint model %1 "main"
    if (earlyAlready) w.insert(w.end(), {Op(3, 16), 1, 9});
    else w.insert(w.end(), {Op(3, 16), 1, 7});              // OriginUpperLeft
    w.insert(w.end(), {Op(3, 71), 5, 42});                  // OpDecorate %5 NoContraction
    w.insert(w.end(), {Op(4, 71), 6, 30, 0});               // OpDecorate %6 Location 0
    w.insert(w.end(), {Op(3, 71), 7, 42});                  // OpDecorate %7 NoContraction
    w.insert(w.end(), {Op(2, 19), 2});                      // OpTypeVoid %2
    return w;
}
int main()
{
    {
        auto w = Module(4, false);
        const size_t before = w.size();
        CHECK(st::StripNoContraction(w) == 2);
        CHECK(w.size() == before - 6);
        for (size_t i = 5; i < w.size();) {
            const uint32_t count = w[i] >> 16, opcode = w[i] & 0xFFFF;
            CHECK(!(opcode == 71 && count == 3 && w[i + 2] == 42));
            i += count ? count : 1;
        }
        CHECK(st::StripNoContraction(w) == 0);
    }
    {
        const auto w = Module(4, false);
        std::vector<uint32_t> early;
        CHECK(st::AddEarlyFragmentTests(w, early));
        CHECK(early.size() == w.size() + 3);
        // Inserted right after the entry point: header 5 + capability 2 + memory model 3 + entry point 5 = 15.
        CHECK(early[15] == Op(3, 16) && early[16] == 1 && early[17] == 9);
        std::vector<uint32_t> again;
        CHECK(!st::AddEarlyFragmentTests(early, again));
    }
    {
        std::vector<uint32_t> out;
        CHECK(!st::AddEarlyFragmentTests(Module(0, false), out)); // vertex
        CHECK(!st::AddEarlyFragmentTests(Module(4, true), out));  // already early
        std::vector<uint32_t> bad{1, 2, 3};
        CHECK(!st::AddEarlyFragmentTests(bad, out));
        CHECK(st::StripNoContraction(bad) == 0);
    }
    if (failures) return 1;
    std::puts("spirv_lean_test: ok");
    return 0;
}

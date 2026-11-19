// Turn 58: GameTimeNs lock-free fast path (debug/fast_forward.h) agrees with the locked clock, is not used while
// paused or fast-forwarding, and never runs backwards on a thread.
// g++ -std=c++20 -O2 -pthread -I LostOdysseyRecomp tools/tests/fast_forward_steady_test.cpp
#include <debug/fast_forward.h>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
using namespace debug_menu::fast_forward;
int main() {
    uint64_t prev = GameTimeNs(); // initializes, publishes
    if (steadyOffset.load() == Clock::kNoOffset) { puts("FAIL not published"); return 1; }
    for (int i = 0; i < 100000; ++i) { uint64_t v = GameTimeNs(); if (v < prev) { puts("FAIL backwards"); return 1; } prev = v; }
    // fast path agrees with the locked clock
    uint64_t fast = GameTimeNs();
    uint64_t locked; { std::lock_guard l(mutex); locked = control.clock.Read(NowNs()); }
    if (locked < fast || locked - fast > 5'000'000) { printf("FAIL mismatch %llu\n", (unsigned long long)(locked - fast)); return 1; }
    SetPaused(true);
    if (steadyOffset.load() != Clock::kNoOffset) { puts("FAIL published while paused"); return 1; }
    uint64_t p1 = GameTimeNs(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); uint64_t p2 = GameTimeNs();
    if (p2 != p1) { puts("FAIL clock ran while paused"); return 1; }
    SetPaused(false);
    uint64_t r1 = GameTimeNs(); if (r1 < p2) { puts("FAIL backwards after resume"); return 1; }
    Enable(true); SetRate(4); Sample(200, 0, true); Sample(0,0,true); Sample(200,0,true); // hold -> boosted
    if (steadyOffset.load() != Clock::kNoOffset) { puts("FAIL published while boosted"); return 1; }
    uint64_t b1 = GameTimeNs(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); uint64_t b2 = GameTimeNs();
    if (b2 - b1 < 150'000'000ull) { printf("FAIL boost not applied %llu\n",(unsigned long long)(b2-b1)); return 1; }
    Sample(0, 0, true); // release -> rate 1
    uint64_t a = GameTimeNs(); if (a < b2) { puts("FAIL backwards after boost"); return 1; }
    if (steadyOffset.load() == Clock::kNoOffset) { puts("FAIL not republished"); return 1; }
    // threads
    std::vector<std::thread> ts; std::atomic<bool> bad{false};
    for (int t = 0; t < 4; ++t) ts.emplace_back([&]{ uint64_t p = 0; for (int i = 0; i < 200000; ++i) { uint64_t v = GameTimeNs(); if (v < p) bad = true; p = v; if (i % 5000 == 0) Sample(i % 10000 ? 0 : 200, 0, true);} });
    for (auto& t : ts) t.join();
    if (bad) { puts("FAIL per-thread backwards"); return 1; }
    puts("ok");
}

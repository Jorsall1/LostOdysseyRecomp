// gpu/gpu_busy.h: overlapping batches count once (Switch 2 gpu_pct 158, 2026-11-18).
#include <gpu/gpu_busy.h>
#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main()
{
    using gpu::gpu_busy::BusyUnion;
    {
        BusyUnion u; // disjoint
        u.Add(100, 200);
        u.Add(300, 350);
        CHECK(u.total == 150);
    }
    {
        BusyUnion u; // overlapping: [100,200) and [150,260) cover 160
        u.Add(100, 200);
        u.Add(150, 260);
        CHECK(u.total == 160);
    }
    {
        BusyUnion u; // nested batch adds nothing
        u.Add(100, 400);
        CHECK(u.Add(150, 300) == 0);
        CHECK(u.total == 300);
    }
    {
        BusyUnion u; // out of order: a renderer batch read after a later present still counts
        u.Add(500, 600);
        CHECK(u.Add(100, 200) == 100);
        CHECK(u.total == 200);
        CHECK(u.Add(150, 550) == 300); // fills the gap [200,500) only
        CHECK(u.total == 500);
        CHECK(u.Add(120, 580) == 0);
        CHECK(u.intervals.size() == 1);
    }
    {
        BusyUnion u; // the Switch 2 pattern: present lists first, the 11 ms batches two frames later
        uint64_t t = 1'000'000'000;
        for (int frame = 0; frame < 10; ++frame) {
            const uint64_t start = t + uint64_t(frame) * 16'000'000;
            u.Add(start + 12'000'000, start + 13'000'000);          // present list
            if (frame >= 2) {
                const uint64_t earlier = t + uint64_t(frame - 2) * 16'000'000;
                u.Add(earlier, earlier + 11'000'000);               // renderer batch
            }
        }
        CHECK(u.total == 10ull * 1'000'000 + 8ull * 11'000'000);
    }
    {
        BusyUnion u; // old intervals are forgotten, the list stays bounded
        for (uint64_t i = 1; i <= 2000; ++i) u.Add(i * 10'000'000, i * 10'000'000 + 1'000'000);
        CHECK(u.total == 2000ull * 1'000'000);
        CHECK(u.intervals.size() <= BusyUnion::kMaxIntervals);
    }
    {
        BusyUnion u; // invalid results
        CHECK(u.Add(0, 100) == 0);
        CHECK(u.Add(200, 200) == 0);
        CHECK(u.Add(300, 250) == 0);
        CHECK(u.total == 0 && u.intervals.empty());
    }
    {
        BusyUnion u; // a torn result spanning seconds is ignored and does not swallow later batches
        CHECK(u.Add(1'000, 5'000'000'000) == 0);
        CHECK(u.Add(2'000'000, 3'000'000) == 1'000'000);
    }
    {
        // The global path: total and longest batch.
        gpu::gpu_busy::AddBatch(1000, 2000);
        gpu::gpu_busy::AddBatch(1500, 4000);
        CHECK(gpu::gpu_busy::Total() == 3000);
        CHECK(gpu::gpu_busy::TakeMaxBatchNs() == 2500);
        CHECK(gpu::gpu_busy::PeekMaxBatchNs() == 0);
    }
    if (failures) return EXIT_FAILURE;
    std::puts("gpu_busy_test: ok");
    return 0;
}

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
        BusyUnion u; // out of order: an earlier batch after a later one never goes negative
        u.Add(500, 600);
        CHECK(u.Add(100, 200) == 0);
        CHECK(u.total == 100);
    }
    {
        BusyUnion u; // invalid results
        CHECK(u.Add(0, 100) == 0);
        CHECK(u.Add(200, 200) == 0);
        CHECK(u.Add(300, 250) == 0);
        CHECK(u.total == 0 && u.lastEnd == 0);
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

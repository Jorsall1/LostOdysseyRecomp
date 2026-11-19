// gpu/submission_ring.h: the device-loss ring and the Switch 2 draw split.
#include <gpu/submission_ring.h>
#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main()
{
    using namespace gpu::submission_ring;
    {
        Ring<4> ring;
        CHECK(ring.Size() == 0);
        for (uint64_t s = 1; s <= 3; ++s) ring.Push({s, s * 10});
        CHECK(ring.Size() == 3 && ring.At(0).serial == 1 && ring.At(2).serial == 3);
        for (uint64_t s = 4; s <= 6; ++s) ring.Push({s, s * 10});
        // Wrapped: keeps the newest four, oldest first.
        CHECK(ring.Size() == 4);
        CHECK(ring.At(0).serial == 3 && ring.At(1).serial == 4 && ring.At(3).serial == 6);
        ring.Complete(5, 1234);
        CHECK(ring.At(2).completed && ring.At(2).gpuUs == 1234);
        CHECK(!ring.At(3).completed);
        ring.Complete(1, 99); // already overwritten: nothing changes
        for (size_t i = 0; i < ring.Size(); ++i) CHECK(ring.At(i).gpuUs != 99);
    }
    {
        CHECK(BatchDrawLimit("0") == 0);
        CHECK(BatchDrawLimit("512") == 512);
        CHECK(BatchDrawLimit("8") == 64);
        CHECK(BatchDrawLimit("abc") == kDefaultBatchDraws);
        CHECK(BatchDrawLimit(nullptr) == kDefaultBatchDraws);
        CHECK(!SplitBeforeDraw(5000, 0));
        CHECK(!SplitBeforeDraw(1023, 1024) && SplitBeforeDraw(1024, 1024));
#ifdef __SWITCH2__
        CHECK(kDefaultBatchDraws == 1024);
#else
        CHECK(kDefaultBatchDraws == 0);
#endif
    }
    if (failures) return EXIT_FAILURE;
    std::puts("submission_ring_test: ok");
    return 0;
}

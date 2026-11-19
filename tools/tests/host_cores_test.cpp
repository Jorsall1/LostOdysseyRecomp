// gpu/host_cores.h: pipeline workers on cores 3-5 when the Switch 2 process may use them.
#include <gpu/host_cores.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

// renderer.cpp's prefetch pool sizing: max(1, min(HostWorkerCap(logical), logical / 2)), HostWorkerCap = logical - 1.
static size_t Workers(unsigned logical) { return std::max<size_t>(1, std::min<size_t>(logical - 1, logical / 2)); }

int main()
{
    using namespace gpu::host_cores;
    CHECK(LogicalThreads(0x3f) == 6 && WorkersOffGuestCores(0x3f));
    CHECK(LogicalThreads(0x07) == 3 && !WorkersOffGuestCores(0x07));
    CHECK(LogicalThreads(0x0f) == 3 && !WorkersOffGuestCores(0x0f)); // core 3 alone is not enough
    CHECK(LogicalThreads(0) == 3);
    CHECK(Workers(LogicalThreads(0x3f)) == 3);
    CHECK(Workers(LogicalThreads(0x07)) == 1);
    CHECK((kWorkerCores & kGuestCores) == 0);
    if (failures) return EXIT_FAILURE;
    std::puts("host_cores_test: ok");
    return 0;
}

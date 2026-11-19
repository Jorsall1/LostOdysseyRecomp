// os/switch/ouncevk_channel_tap.c: see ouncevk_channel_tap.h.
// A pass-through around libounce's nvIoctl. Only the channel error
// notification query is looked at; every request goes to the real nvIoctl
// unchanged, and one extra request (the channel's error info) is sent on the
// same nv session, once, after the first reported error.
#if defined(__SWITCH__) && defined(__SWITCH2__)
#include "ouncevk_channel_tap.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t Result;
Result __real_nvIoctl(uint32_t fd, uint32_t request, void* argp);

enum {
    kGetErrorNotification = 0xc0104817u, // NvNotification, 16 bytes, read/write
    kGetErrorInfo = 0x80804816u,         // NvError, 128 bytes, read
    kNoFd = 0xffffffffu,
};

typedef struct {
    uint64_t timestamp;
    uint32_t info32;
    uint16_t info16;
    uint16_t status;
} Notification;

static atomic_uint g_lastFd = kNoFd;
static atomic_int g_claimed;
static atomic_int g_published;
static LoChannelError g_error;

const char* lo_ouncevk_channel_error_name(uint32_t info32)
{
    switch (info32) {
        case 8: return "FIFO idle timeout";
        case 13: return "GR exception / SW notify";
        case 24: return "GR semaphore timeout";
        case 25: return "GR illegal notify";
        case 31: return "MMU fault";
        case 32: return "PBDMA error";
        case 43: return "reset channel verify error";
        case 80: return "PBDMA pushbuffer CRC mismatch";
        default: return "unknown";
    }
}

static void Record(uint32_t fd, Result rc, const Notification* n)
{
    if (atomic_exchange(&g_claimed, 1)) return;
    LoChannelError e;
    memset(&e, 0, sizeof(e));
    e.recorded = 1;
    e.fd = fd;
    e.notificationRc = rc;
    if (n) {
        e.timestamp = n->timestamp;
        e.info32 = n->info32;
        e.info16 = n->info16;
        e.status = n->status;
    }
    e.errorInfoRc = __real_nvIoctl(fd, kGetErrorInfo, e.errorInfo);
    g_error = e;
    atomic_store(&g_published, 1);
    fprintf(stderr,
        "[gpu channel] error notification fd %u rc 0x%x: info32 %u (%s) info16 0x%x status 0x%x timestamp %llu\n",
        fd, rc, e.info32, lo_ouncevk_channel_error_name(e.info32), e.info16, e.status,
        (unsigned long long)e.timestamp);
    fprintf(stderr, "[gpu channel] error info rc 0x%x:", e.errorInfoRc);
    for (int i = 0; i < 32; ++i) fprintf(stderr, " %08x", e.errorInfo[i]);
    fprintf(stderr, "\n");
}

Result __wrap_nvIoctl(uint32_t fd, uint32_t request, void* argp)
{
    const Result rc = __real_nvIoctl(fd, request, argp);
    if (request == kGetErrorNotification && argp) {
        atomic_store_explicit(&g_lastFd, fd, memory_order_relaxed);
        const Notification* n = (const Notification*)argp;
        // The driver treats a failed query or a non-zero status as an error.
        if (rc != 0 || n->status != 0) Record(fd, rc, rc == 0 ? n : NULL);
    }
    return rc;
}

int lo_ouncevk_channel_error(LoChannelError* out)
{
    if (!atomic_load(&g_published)) {
        const uint32_t fd = atomic_load_explicit(&g_lastFd, memory_order_relaxed);
        if (fd != kNoFd && !atomic_load(&g_claimed)) {
            Notification n;
            memset(&n, 0, sizeof(n));
            const Result rc = __real_nvIoctl(fd, kGetErrorNotification, &n);
            if (rc != 0 || n.status != 0) Record(fd, rc, rc == 0 ? &n : NULL);
        }
    }
    if (!atomic_load(&g_published)) {
        if (out) memset(out, 0, sizeof(*out));
        return 0;
    }
    if (out) *out = g_error;
    return 1;
}
#endif

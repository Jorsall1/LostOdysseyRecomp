// os/switch/ouncevk_channel_tap.h
// Switch 2 only. Records the GPU channel's error notification the first time
// OunceVK's channel health check (nvIoctl 0xc0104817) reports an error, plus
// the channel's error info (nvIoctl 0x80804816). The driver keeps only the
// notification's status word and drops the error type (info32: 8 idle
// timeout, 13 GR exception, 31 MMU fault, 32 PBDMA error, ...); this keeps it.
// Wired in with -Wl,--wrap=nvIoctl (LostOdysseyRecomp/CMakeLists.txt).
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct LoChannelError {
    int32_t recorded;           // 0: no error seen yet
    uint32_t fd;                // channel fd the driver queried
    uint32_t notificationRc;    // nvIoctl result of the health query
    uint64_t timestamp;         // notification timestamp (GPU time)
    uint32_t info32;            // error type (NvNotificationType)
    uint16_t info16;
    uint16_t status;            // 0xffff after an error
    uint32_t errorInfoRc;       // nvIoctl result of the error info query
    uint32_t errorInfo[32];     // NvError: type + 31 words
} LoChannelError;
// Copies the recorded error. When none was recorded yet and the driver has
// queried a channel before, asks that channel now (for stops the driver
// reports without a health check, e.g. a refused submit). Returns recorded.
int lo_ouncevk_channel_error(LoChannelError* out);
// Readable name of info32.
const char* lo_ouncevk_channel_error_name(uint32_t info32);
#ifdef __cplusplus
}
#endif

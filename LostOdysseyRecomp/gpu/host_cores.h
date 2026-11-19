#pragma once
#include <cstdint>

// The renderer's worker pools on the Switch (gpu/renderer.cpp HostLogicalThreads). The guest threads run on cores
// 0-2 (LO_GUEST_AFFINITY). Switch 2 under kaiHEN may also use cores 3-5 (process core mask 0x3f, stderr "thread
// placement"): pipeline preparation then gets three workers there instead of one beside the game (turn 35; startup
// preparation took about 40 s on one).
namespace gpu::host_cores
{
inline constexpr uint64_t kGuestCores = 0x07;  // 0-2
inline constexpr uint64_t kWorkerCores = 0x38; // 3-5
// Logical threads the pools size themselves from: 6 with cores 3-5 available, else the three application cores.
inline constexpr unsigned LogicalThreads(uint64_t processMask)
{
    return (processMask & kWorkerCores) == kWorkerCores ? 6u : 3u;
}
// Whether worker threads go to cores 3-5.
inline constexpr bool WorkersOffGuestCores(uint64_t processMask)
{
    return (processMask & kWorkerCores) == kWorkerCores;
}
}

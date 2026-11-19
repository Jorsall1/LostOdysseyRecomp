#pragma once
#include <array>
#include <cstdint>
#include <atomic>
#include <mutex>
// The renderer's last GPU operations, tagged with the batch that records them, so a GPU stop names what the batch
// that faulted contained (gpu/renderer.cpp LogSubmissionRing prints the oldest unfinished batch and the refused one).
// One fixed array, no allocation. Only the renderer thread records, without a lock (turn 57: the lock was taken
// for every draw, ~2100 a frame in the busy street); the reader (a GPU stop) takes `mutex` against other readers and
// may see a record being written, which only garbles that one line of a diagnostic dump.
namespace gpu::op_ring
{
enum class Kind : uint8_t { Draw, Clear, Resolve, Transfer, Copy };
inline const char* Name(Kind kind)
{
    switch (kind) {
    case Kind::Draw: return "draw";
    case Kind::Clear: return "clear";
    case Kind::Resolve: return "resolve";
    case Kind::Transfer: return "transfer";
    default: return "copy";
    }
}
struct Op
{
    uint64_t batch = 0;
    Kind kind = Kind::Draw;
    uint8_t prim = 0;
    uint16_t flags = 0;
    uint32_t vs = 0, ps = 0;              // low 32 bits of the shader hashes (draws)
    uint32_t count = 0;                   // indices (draws), ops (clears), rows (copies)
    uint32_t a = 0, aw = 0, ah = 0;       // colour / destination: serial, physical size
    uint32_t b = 0, bw = 0, bh = 0;       // depth / source: serial, physical size
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; // scissor or region
    uint32_t vw = 0, vh = 0;              // viewport size (draws)
};
inline constexpr size_t kOps = 8192;
struct Ring
{
    std::mutex mutex;
    std::array<Op, kOps> ops{};
    std::atomic<uint64_t> next{0};
    void Record(const Op& op)
    {
        const uint64_t index = next.load(std::memory_order_relaxed);
        ops[index % kOps] = op;
        next.store(index + 1, std::memory_order_release);
    }
};
inline Ring g_ring;
}

// SEH __finally blocks that unlock the guest heap (Switch 2 loading hang, turn 21).
//
// The game's heap allocator (sub_823ACCB0) and two other heap functions take the
// heap's critical section (*(heap + 1408), 0x510610) inside __try and unlock it
// in a __finally block. MSVC compiles the __finally as a separate funclet that
// the parent calls with r12 = its frame + N and that reads the parent's
// non-volatile registers directly: r22/r27 for sub_823ACCB0, r25/r30 for
// sub_823ADE28, r22 for sub_827CCF80. On the 360 those registers are shared.
// In the recompiled code every function's r12..r31 are C++ locals that start at
// zero, so the funclet always sees "lock not taken" (or a null heap) and never
// unlocks. Each allocation that reaches the funclet path (the large-block path
// through NtAllocateVirtualMemory, and the failure paths) left one more level
// of recursion on the heap lock.
//
// Hardware evidence: Guest Main held 0x510610 with recursion 422 when the
// rendering thread blocked on it (2026-11-16). GuestLockHold then handed it
// over during a fence wait, the rendering thread leaked it the same way, and
// Guest Main never got it back in Unpark: runtime-1795013568446298.log shows
// the fence counter at 0 (the fence ran 254 ms after it was queued) while the
// wait never returned, for 13.6 s and on.
//
// MSVC keeps each of those registers mirrored in the parent's frame for the
// exception path (the sibling entry points 0x823AD51C, 0x823AE094 and
// 0x827CD79C load them from there), and every parent writes its slot whenever
// it changes the register. Each parent builds its frame with
// "addi r31,r1,-N; stwu r1,-N(r1)" and no dynamic stack, so at the call r1 is
// the parent's r31. These overrides read the slots and unlock as the original
// funclet would. LO_SEH_FINALLY_FIX=0 runs the generated funclets.
#include <stdafx.h>
#include <cpu/ppc_context.h>
#include <kernel/memory.h>
#include <atomic>
#include <cstdlib>
#include <os/logger.h>

extern "C" PPC_FUNC(__imp__sub_823AD544);
extern "C" PPC_FUNC(__imp__sub_823AE0BC);
extern "C" PPC_FUNC(__imp__sub_827CD7BC);

namespace
{
    bool SehFinallyFix()
    {
        static const bool enabled = [] {
            const char* value = std::getenv("LO_SEH_FINALLY_FIX");
            return !(value && value[0] == '0');
        }();
        return enabled;
    }

    // The heap's section is at *(heap + 1408). Leave it through the import, as
    // the funclet does (RtlLeaveCriticalSection also pops GuestLockHold's entry).
    void LeaveHeapSection(PPCContext& ctx, uint8_t* base, uint32_t heap, uint32_t returnAddress)
    {
        static std::atomic<uint32_t> logged{0};
        if (heap < 0x10000)
        {
            if (logged.fetch_add(1, std::memory_order_relaxed) < 4)
                LOG_WARNING("SEH finally {:#x}: heap pointer {:#x} in the parent frame is not valid; not unlocking",
                    returnAddress, heap);
            return;
        }
        if (logged.fetch_add(1, std::memory_order_relaxed) < 4)
            LOG_INFO("SEH finally {:#x}: unlocking heap section {:#x} (the generated funclet would not)",
                returnAddress, PPC_LOAD_U32(heap + 1408));
        ctx.r3.u64 = PPC_LOAD_U32(heap + 1408);
        ctx.lr = returnAddress;
        __imp__RtlLeaveCriticalSection(ctx, base);
    }
}

// sub_823ACCB0 (heap allocator), frame 320: lock taken (r22) at +104, heap (r27) at +128.
PPC_FUNC(sub_823AD544)
{
    if (!SehFinallyFix())
    {
        __imp__sub_823AD544(ctx, base);
        return;
    }
    const uint32_t frame = ctx.r1.u32;
    if (PPC_LOAD_U32(frame + 104) != 0)
        LeaveHeapSection(ctx, base, PPC_LOAD_U32(frame + 128), 0x823AD570);
}

// sub_823ADE28, frame 176: lock taken (r25) at +84, heap (r30) at +100.
PPC_FUNC(sub_823AE0BC)
{
    if (!SehFinallyFix())
    {
        __imp__sub_823AE0BC(ctx, base);
        return;
    }
    const uint32_t frame = ctx.r1.u32;
    if (PPC_LOAD_U32(frame + 84) != 0)
        LeaveHeapSection(ctx, base, PPC_LOAD_U32(frame + 100), 0x823AE0E8);
}

// sub_827CCF80, frame 320: lock taken at +96 (the funclet reads it from memory,
// but through its zero r31), heap (r22) at +84.
PPC_FUNC(sub_827CD7BC)
{
    if (!SehFinallyFix())
    {
        __imp__sub_827CD7BC(ctx, base);
        return;
    }
    const uint32_t frame = ctx.r1.u32;
    if (PPC_LOAD_U32(frame + 96) != 0)
        LeaveHeapSection(ctx, base, PPC_LOAD_U32(frame + 84), 0x827CD7E8);
}

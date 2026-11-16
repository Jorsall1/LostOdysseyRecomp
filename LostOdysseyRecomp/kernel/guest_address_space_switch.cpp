#include <os/platform.h>
#if LO_PLATFORM_SWITCH
#include "guest_address_space_layout.h"
#include <switch.h>
#include <malloc.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

// Nintendo Switch backend (Horizon OS, libnx). Ported from UnleashedRecomp-NX
// (kernel/memory.cpp) and extended with the Xbox 360 physical aliases.
//
// Horizon has no mmap, no shared anonymous memory and no lazy commit. The
// 4 GiB guest window is a virtmem reservation in the ASLR region; guest pages
// are backed explicitly through Commit() by the allocators that hand them out
// (memory.cpp PageAllocator, heap.cpp, image load). A backing chunk is heap
// memory turned into code memory (svcMapProcessCodeMemory) and then mapped
// into the window with svcMapProcessMemory. The same code-memory source can be
// mapped more than once, which gives the physical views their aliases:
//   A  0xA0000000 + x   backing x
//   C  0xC0000000 + x   backing x          (same pages as A)
//   E  0xE0000000 + y   backing y + 0x1000 (A shifted by one 4 KiB page)
// Both syscalls need the homebrew loader to grant them: launch through title
// takeover (hold R while starting a game), not the album applet.
//
// Switch 2 note: kaiHEN's loader does not fill the syscall hint table, so
// envIsSyscallHinted() reports 0 even when the syscalls work. The backend is
// therefore chosen by probing the syscalls on a real 4 KiB block instead of
// trusting the hints, and a transfer-memory backend covers the case where the
// kernel refuses svcMapProcessCodeMemory.
namespace GuestAddressSpace
{
namespace
{
constexpr size_t kPage = 0x1000;
// Commit granularity. Large chunks keep the number of kernel memory blocks
// low; the guest's own allocators are dense, so little is wasted.
constexpr size_t kChunk = 0x200000;
constexpr size_t kChunkCount = kSize / kChunk;
constexpr unsigned kSvcMapProcessMemory = 0x74;
constexpr unsigned kSvcUnmapProcessMemory = 0x75;
constexpr unsigned kSvcMapProcessCodeMemory = 0x77;
constexpr unsigned kSvcUnmapProcessCodeMemory = 0x78;
constexpr unsigned kSvcSetProcessMemoryPermission = 0x73;
constexpr unsigned kSvcCreateTransferMemory = 0x15;
constexpr unsigned kSvcMapTransferMemory = 0x51;
constexpr unsigned kSvcUnmapTransferMemory = 0x52;

// How committed pages reach the window.
//   ProcessMemory:  heap -> code memory (0x77) -> window (0x74). The only
//     backend that can start a mapping inside a chunk, which the E view needs
//     because guest E+y reads backing y+0x1000.
//   TransferMemory: heap -> transfer memory (0x15) -> window (0x51). A transfer
//     memory always maps from its first page, so the E view is placed one page
//     lower than A instead, and its first page stays unmapped.
enum class Backend { Unchecked, None, ProcessMemory, TransferMemory };

Backend s_backend = Backend::Unchecked;
uint8_t* s_base = nullptr;
VirtmemReservation* s_reservation = nullptr;
Handle s_process = INVALID_HANDLE;
std::mutex s_mutex;
// One flag per 2 MiB chunk of the virtual view and of the A view. C and E are
// committed together with A and never on their own.
bool s_committed[kChunkCount]{};
size_t s_committedBytes = 0;
// Code-memory source (ProcessMemory) or transfer-memory handle (TransferMemory)
// of each committed chunk, under s_mutex, to map a view again (EnsureBacked).
uintptr_t s_source[kChunkCount]{};
RepairInfo s_repair{};
bool s_storeAliasGapLogged = false;
// One line (separated by " | ") for the log and the error dialog: nothing else
// exists during Memory's construction, so a failed boot reports the kernel's
// answers from here.
char s_diagnostics[2048];
size_t s_diagnosticLength = 0;

void Diag(const char* format, ...)
{
    char line[320];
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (written <= 0)
        return;
    const size_t length = size_t(written) < sizeof(line) ? size_t(written) : sizeof(line) - 1;
    std::fprintf(stderr, "guest memory: %s\n", line);
    if (s_diagnosticLength + length + 4 >= sizeof(s_diagnostics))
        return;
    if (s_diagnosticLength != 0)
    {
        std::memcpy(s_diagnostics + s_diagnosticLength, " | ", 3);
        s_diagnosticLength += 3;
    }
    std::memcpy(s_diagnostics + s_diagnosticLength, line, length);
    s_diagnosticLength += length;
    s_diagnostics[s_diagnosticLength] = '\0';
}

#if !defined(__SWITCH2__)
bool ProcessMemorySyscallsAvailable()
{
    return envIsSyscallHinted(kSvcMapProcessMemory) && envIsSyscallHinted(kSvcUnmapProcessMemory) &&
        envIsSyscallHinted(kSvcMapProcessCodeMemory) && envIsSyscallHinted(kSvcUnmapProcessCodeMemory);
}
#endif

#if defined(__SWITCH2__)
// Everything the failure report needs, gathered before anything can log it.
void LogFacts()
{
    Diag("switch 2: hints 0x73=%d 0x74=%d 0x75=%d 0x77=%d 0x78=%d 0x15=%d 0x51=%d 0x52=%d",
        int(envIsSyscallHinted(kSvcSetProcessMemoryPermission)), int(envIsSyscallHinted(kSvcMapProcessMemory)),
        int(envIsSyscallHinted(kSvcUnmapProcessMemory)), int(envIsSyscallHinted(kSvcMapProcessCodeMemory)),
        int(envIsSyscallHinted(kSvcUnmapProcessCodeMemory)), int(envIsSyscallHinted(kSvcCreateTransferMemory)),
        int(envIsSyscallHinted(kSvcMapTransferMemory)), int(envIsSyscallHinted(kSvcUnmapTransferMemory)));

    u64 total = 0, used = 0, aslr = 0, aslrSize = 0, heap = 0, heapSize = 0, alias = 0, aliasSize = 0;
    const Result rcTotal = svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    const Result rcUsed = svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    const Result rcAslr = svcGetInfo(&aslr, InfoType_AslrRegionAddress, CUR_PROCESS_HANDLE, 0);
    const Result rcAslrSize = svcGetInfo(&aslrSize, InfoType_AslrRegionSize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&heap, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&heapSize, InfoType_HeapRegionSize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&alias, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&aliasSize, InfoType_AliasRegionSize, CUR_PROCESS_HANDLE, 0);
    Diag("switch 2: total=%#llx (rc %#x) used=%#llx (rc %#x)", (unsigned long long)total, rcTotal,
        (unsigned long long)used, rcUsed);
    Diag("switch 2: aslr=%#llx+%#llx (rc %#x/%#x) heap-region=%#llx+%#llx alias=%#llx+%#llx",
        (unsigned long long)aslr, (unsigned long long)aslrSize, rcAslr, rcAslrSize, (unsigned long long)heap,
        (unsigned long long)heapSize, (unsigned long long)alias, (unsigned long long)aliasSize);

    const struct mallinfo memory = mallinfo();
    Diag("switch 2: newlib heap %llu MiB in use, %llu MiB arena, window needs %#zx MiB contiguous",
        (unsigned long long)(size_t(memory.uordblks) >> 20), (unsigned long long)(size_t(memory.arena) >> 20),
        kSize >> 20);
}

// Probes the process-memory pair on a scratch page: map 4 KiB of real heap into
// the code region, map that into an ordinary address, read it back, undo both.
// Every result code is reported, so the hint table is never the answer.
bool ProbeProcessMemory(Handle process)
{
    void* scratch = memalign(kPage, kPage);
    if (!scratch)
    {
        Diag("probe process memory: no 4 KiB scratch allocation");
        return false;
    }
    std::memset(scratch, 0x5A, kPage);

    Result mapCode = 0, setPermission = 0, mapView = 0;
    void* code = nullptr;
    void* view = nullptr;
    virtmemLock();
    code = virtmemFindCodeMemory(kPage, kPage);
    view = virtmemFindAslr(kPage, kPage);
    if (code && view)
    {
        mapCode = svcMapProcessCodeMemory(process, uintptr_t(code), uintptr_t(scratch), kPage);
        if (R_SUCCEEDED(mapCode))
        {
            setPermission = svcSetProcessMemoryPermission(process, uintptr_t(code), kPage, Perm_Rw);
            mapView = svcMapProcessMemory(view, process, uintptr_t(code), kPage);
        }
    }
    virtmemUnlock();
    Diag("probe process memory: code=%p view=%p code=%#x perm=%#x map=%#x", code, view, mapCode, setPermission,
        mapView);

    bool ok = code && view && R_SUCCEEDED(mapCode) && R_SUCCEEDED(mapView);
    if (ok)
    {
        // A read-only mapping would let the guest read but not write, so the
        // permission result decides whether the probe succeeded.
        volatile uint8_t* bytes = static_cast<uint8_t*>(view);
        ok = bytes[0] == 0x5A && bytes[kPage - 1] == 0x5A;
        if (ok && R_SUCCEEDED(setPermission))
        {
            bytes[0] = 0xA5;
            ok = bytes[0] == 0xA5;
        }
        Diag("probe process memory: %s", ok ? "usable" : "mapping or permission unusable");
    }
    if (R_SUCCEEDED(mapView))
        svcUnmapProcessMemory(view, process, uintptr_t(code), kPage);
    if (R_SUCCEEDED(mapCode))
        svcUnmapProcessCodeMemory(process, uintptr_t(code), uintptr_t(scratch), kPage);
    // scratch, code and view are leaked on purpose: after a partial failure the
    // kernel may still own those pages and freeing them would corrupt the heap.
    return ok;
}

// Transfer memory is the fallback when the kernel refuses code memory: the
// heap page is handed to the kernel (0x15) and mapped back (0x51). It needs no
// process handle and no code region, but it cannot start inside a chunk.
bool ProbeTransferMemory()
{
    void* scratch = memalign(kPage, kPage);
    if (!scratch)
    {
        Diag("probe transfer memory: no 4 KiB scratch allocation");
        return false;
    }
    std::memset(scratch, 0x3C, kPage);

    Handle memory = INVALID_HANDLE;
    const Result created = svcCreateTransferMemory(&memory, scratch, kPage, Perm_Rw);
    void* view = nullptr;
    Result mapped = 0;
    if (R_SUCCEEDED(created))
    {
        virtmemLock();
        view = virtmemFindAslr(kPage, kPage);
        virtmemUnlock();
        if (view)
            mapped = svcMapTransferMemory(memory, view, kPage, Perm_Rw);
    }
    Diag("probe transfer memory: create=%#x view=%p map=%#x", created, view, mapped);

    bool ok = R_SUCCEEDED(created) && view && R_SUCCEEDED(mapped);
    if (ok)
    {
        volatile uint8_t* bytes = static_cast<uint8_t*>(view);
        ok = bytes[0] == 0x3C && bytes[kPage - 1] == 0x3C;
        if (ok)
        {
            bytes[0] = 0xC3;
            ok = bytes[0] == 0xC3;
        }
        Diag("probe transfer memory: %s", ok ? "usable" : "mapping unusable");
        svcUnmapTransferMemory(memory, view, kPage);
    }
    if (R_SUCCEEDED(created))
        svcCloseHandle(memory); // The pages stay with the kernel; scratch is not freed.
    return ok;
}
#endif

struct HandleReceive
{
    Handle session;
    Handle received;
};

void ReceiveOwnProcessHandle(void* arg)
{
    auto& receive = *static_cast<HandleReceive*>(arg);
    void* tls = armGetTls();
    hipcMakeRequestInline(tls);
    s32 index = 0;
    if (R_SUCCEEDED(svcReplyAndReceive(&index, &receive.session, 1, INVALID_HANDLE, UINT64_MAX)))
    {
        const HipcParsedRequest request = hipcParseRequest(tls);
        if (request.meta.num_copy_handles == 1)
            receive.received = request.data.copy_handles[0];
    }
    svcCloseHandle(receive.session);
}

// The process syscalls need a real handle to this process; they reject the
// CUR_PROCESS_HANDLE pseudo-handle. hbloader passes one. Loaders that do not
// (Ryujinx runs an NRO directly) get one the way hbloader does: send the
// pseudo-handle to ourselves over a session, and the kernel copies a real one.
Handle OwnProcessHandle()
{
    if (const Handle own = envGetOwnProcessHandle(); own != INVALID_HANDLE)
        return own;
    Handle server = INVALID_HANDLE, client = INVALID_HANDLE;
    if (R_FAILED(svcCreateSession(&server, &client, 0, 0)))
        return INVALID_HANDLE;
    HandleReceive receive{ server, INVALID_HANDLE };
    Thread thread;
    if (R_FAILED(threadCreate(&thread, ReceiveOwnProcessHandle, &receive, nullptr, 0x1000, 0x20, -2)) ||
        R_FAILED(threadStart(&thread)))
    {
        svcCloseHandle(server);
        svcCloseHandle(client);
        return INVALID_HANDLE;
    }
    hipcMakeRequestInline(armGetTls(), .num_copy_handles = 1).copy_handles[0] = CUR_PROCESS_HANDLE;
    svcSendSyncRequest(client); // The receiver closes the session instead of replying.
    svcCloseHandle(client);
    threadWaitForExit(&thread);
    threadClose(&thread);
    return receive.received;
}

// Maps [source + sourceOffset, ...) of one chunk into the window at
// windowOffset. A transfer memory can only be mapped from its first page.
bool MapView(size_t windowOffset, uintptr_t source, size_t sourceOffset, size_t length, int32_t view,
             size_t chunkOffset)
{
    if (length == 0)
        return true;
    Result rc = 0;
    if (s_backend == Backend::TransferMemory)
    {
        if (sourceOffset != 0)
            rc = 0xFFFFFFFFu;
        else
            rc = svcMapTransferMemory(Handle(source), s_base + windowOffset, length, Perm_Rw);
    }
    else
    {
        rc = svcMapProcessMemory(s_base + windowOffset, s_process, source + sourceOffset, length);
    }
    if (R_FAILED(rc))
        RecordFailure(FailureOperation::MapView, rc, view, s_base + windowOffset, length, chunkOffset);
    return R_SUCCEEDED(rc);
}

// Backs [offset, offset + size) of the window with fresh zeroed memory and,
// for the A view, maps the same pages at the C and E aliases.
bool MapChunk(size_t offset, size_t size, uintptr_t& sourceOut)
{
    void* backing = memalign(kPage, size);
    if (!backing)
    {
        RecordFailure(FailureOperation::CreateBacking, 0, -1, nullptr, size, offset);
        return false;
    }
    std::memset(backing, 0, size);

    uintptr_t source = 0;
    Result rc = 0;
    if (s_backend == Backend::TransferMemory)
    {
        Handle memory = INVALID_HANDLE;
        rc = svcCreateTransferMemory(&memory, backing, size, Perm_Rw);
        if (R_FAILED(rc))
        {
            Diag("chunk %#zx: svcCreateTransferMemory(%#zx) failed: %#x", offset, size, rc);
            RecordFailure(FailureOperation::CreateBacking, rc, -1, backing, size, offset);
            return false; // backing stays leaked: the kernel may own part of it
        }
        source = uintptr_t(memory);
    }
    else
    {
        // The address is a random free slice, and the mapping can still fail there (a chunk failed about once in 100
        // runs, at any time, with most of the heap free, and the game then wrote through a null allocation). Another
        // slice works.
        constexpr int kMapAttempts = 16;
        void* code = nullptr;
        for (int attempt = 0; attempt < kMapAttempts; ++attempt)
        {
            virtmemLock();
            code = virtmemFindCodeMemory(size, kPage);
            bool mapped = false;
            if (code)
            {
                rc = svcMapProcessCodeMemory(s_process, uintptr_t(code), uintptr_t(backing), size);
                mapped = R_SUCCEEDED(rc);
                if (mapped)
                {
                    rc = svcSetProcessMemoryPermission(s_process, uintptr_t(code), size, Perm_Rw);
                    if (R_FAILED(rc))
                    {
                        svcUnmapProcessCodeMemory(s_process, uintptr_t(code), uintptr_t(backing), size);
                        mapped = false;
                    }
                }
            }
            virtmemUnlock();
            if (mapped)
                break;
            std::fprintf(stderr, "guest memory: chunk %#zx attempt %d failed at %p, rc %#x\n", offset, attempt, code,
                rc);
            code = nullptr;
        }
        if (!code)
        {
            RecordFailure(FailureOperation::CreateBacking, rc, -1, code, size, offset);
            free(backing);
            return false;
        }
        source = uintptr_t(code);
    }

    bool ok = MapView(offset, source, 0, size, offset < kStarts[1] ? 0 : 1, offset);
    if (ok && offset >= kStarts[1] && offset < kStarts[2])
    {
        const size_t physical = offset - kStarts[1];
        ok = MapView(kStarts[2] + physical, source, 0, size, 2, offset);
        if (ok)
        {
            if (s_backend == Backend::TransferMemory)
            {
                // A transfer memory maps from its first page, so the E view is
                // placed one page lower than A. The guest's first E page needs
                // a 4 KiB source offset and is the one page left unmapped.
                if (physical == 0)
                {
                    if (!s_storeAliasGapLogged)
                    {
                        s_storeAliasGapLogged = true;
                        Diag("transfer memory backend: E %#zx-%#zx stays unmapped (needs a 4 KiB source offset)",
                            kStarts[3], kStarts[3] + kChunk - kPage);
                    }
                }
                else
                {
                    ok = MapView(kStarts[3] + physical - kPage, source, 0, size, 3, offset);
                }
            }
            else
            {
                // E: guest E+y reads backing y+0x1000. The first page of the
                // first chunk has no E address (it would fall into C).
                const size_t skip = physical == 0 ? kPage : 0;
                ok = MapView(kStarts[3] + physical + skip - kPage, source, skip, size - skip, 3, offset);
            }
        }
    }
    // Mapped views are never released: guest memory lives for the whole run.
    if (ok)
        sourceOut = source;
    return ok;
}
}

uint8_t* Allocate()
{
    ClearFailure();
    s_diagnosticLength = 0;
    s_diagnostics[0] = '\0';
    s_backend = Backend::Unchecked;

    s_process = OwnProcessHandle();
#ifdef __SWITCH2__
    Diag("switch 2: own process handle env=%#x resolved=%#x", envGetOwnProcessHandle(), s_process);
    LogFacts();
#else
    if (!ProcessMemorySyscallsAvailable())
    {
        // Typical cause: started from the album applet. Logged by main.cpp.
        RecordFailure(FailureOperation::ReserveAny, 0xFFFFFFFFu, -1, nullptr, kSize);
        return nullptr;
    }
    if (s_process == INVALID_HANDLE)
    {
        RecordFailure(FailureOperation::ReserveAny, 0xFFFFFFFEu, -1, nullptr, kSize);
        return nullptr;
    }
    s_backend = Backend::ProcessMemory;
#endif

    virtmemLock();
    s_base = static_cast<uint8_t*>(virtmemFindAslr(kSize, kChunk));
    if (s_base)
        s_reservation = virtmemAddReservation(s_base, kSize);
    virtmemUnlock();
    if (!s_base || !s_reservation)
    {
        Diag("window: %#zx MiB at %p not reserved (reservation %p)", kSize >> 20, s_base, s_reservation);
        RecordFailure(FailureOperation::ReserveAny, 0, -1, s_base, kSize);
        s_base = nullptr;
        return nullptr;
    }
    Diag("window: reserved %#zx MiB at %p", kSize >> 20, s_base);

#ifdef __SWITCH2__
    // The hints are not trusted: a loader that does not fill the table still
    // lets the syscalls through, so ask the kernel with a real mapping.
    if (s_process != INVALID_HANDLE && ProbeProcessMemory(s_process))
        s_backend = Backend::ProcessMemory;
    else if (ProbeTransferMemory())
        s_backend = Backend::TransferMemory;
    else
        s_backend = Backend::None;

    if (s_backend == Backend::None)
    {
        Diag("backend: none, the kernel refused process memory and transfer memory");
        RecordFailure(FailureOperation::ReserveAny, s_process == INVALID_HANDLE ? 0xFFFFFFFEu : 0xFFFFFFFFu, -1,
                      nullptr, kSize);
        virtmemLock();
        virtmemRemoveReservation(s_reservation);
        virtmemUnlock();
        s_reservation = nullptr;
        s_base = nullptr;
        return nullptr;
    }
    Diag("backend: %s", s_backend == Backend::ProcessMemory ? "process memory (0x77/0x74)"
                                                            : "transfer memory (0x15/0x51)");
#endif
    // The guest null page stays unmapped: chunk 0 is committed from 0x1000.
    return s_base;
}

void Release(uint8_t* base)
{
    // The process exits with its mappings; nothing to undo early.
    (void)base;
}

bool Commit(uint32_t address, uint32_t size)
{
    if (!s_base || size == 0)
        return s_base != nullptr;
    const uint64_t end = uint64_t(address) + size;
    if (end > kStarts[2])
        return false; // C and E are aliases of A and are committed with it.

    std::lock_guard lock(s_mutex);
    for (uint64_t chunk = address / kChunk; chunk * kChunk < end; ++chunk)
    {
        if (s_committed[chunk])
            continue;
        size_t offset = size_t(chunk * kChunk);
        size_t length = kChunk;
        if (offset == 0)
        {
            // Keep the guest null page unmapped so a null access faults.
            offset = kPage;
            length -= kPage;
        }
        uintptr_t source = 0;
        if (!MapChunk(offset, length, source))
            return false;
        s_source[chunk] = source;
        __atomic_store_n(&s_committed[chunk], true, __ATOMIC_RELEASE); // read without the lock by EnsureBacked
        s_committedBytes += kChunk;
    }
    return true;
}

Backing EnsureBacked(uint32_t address, uint32_t size)
{
    if (!s_base || address < kPage || size == 0)
        return Backing::Failed;
    // C and E alias A: commit the A pages behind them.
    uint64_t start = address;
    if (start >= kStarts[3])
        start = start - kStarts[3] + kStarts[1] + kPage;
    else if (start >= kStarts[2])
        start = start - kStarts[2] + kStarts[1];
    const uint64_t end = start + size;
    if (end > kStarts[2])
        return Backing::Failed;
    bool backed = true;
    for (uint64_t chunk = start / kChunk; chunk * kChunk < end; ++chunk)
        backed = backed && __atomic_load_n(&s_committed[chunk], __ATOMIC_ACQUIRE);
    if (!backed)
        return Commit(uint32_t(start), uint32_t(end - start)) ? Backing::NewlyBacked : Backing::Failed;

    // Committed, so it should be mapped. Entering Uhra, reads of committed chunks still faulted
    // (no backing was needed or failed, yet the page was not there): check with the kernel, and
    // map a missing view again onto the chunk's own source (its C and E aliases keep the data).
    Backing result = Backing::AlreadyBacked;
    for (uint64_t chunk = start / kChunk; chunk * kChunk < end; ++chunk)
    {
        const uint64_t probe = std::max<uint64_t>(start, chunk * kChunk);
        MemoryInfo info{};
        u32 pageInfo = 0;
        if (R_FAILED(svcQueryMemory(&info, &pageInfo, uintptr_t(s_base + probe))))
            continue;
        if (info.type != MemType_Unmapped && (info.perm & Perm_R) != 0)
            continue;
        // Mapped but lent to an IPC request (an SD card read into guest memory):
        // the kernel takes the pages' permissions until the reply. Wait for it
        // (up to 2 s) instead of mapping over it.
        if (info.type != MemType_Unmapped && (info.attr & MemAttr_IsIpcMapped) != 0)
        {
            for (int wait = 0; wait < 20000 && (info.attr & MemAttr_IsIpcMapped) != 0 && (info.perm & Perm_R) == 0; ++wait)
            {
                svcSleepThread(100'000);
                if (R_FAILED(svcQueryMemory(&info, &pageInfo, uintptr_t(s_base + probe))))
                    break;
            }
            if ((info.perm & Perm_R) != 0)
                continue;
        }

        std::lock_guard lock(s_mutex);
        RepairInfo repair{};
        repair.address = uint32_t(probe);
        repair.type = info.type;
        repair.attribute = info.attr;
        repair.permission = info.perm;
        repair.regionStart = info.addr >= uintptr_t(s_base) ? uint64_t(info.addr - uintptr_t(s_base)) : ~0ull;
        repair.regionSize = info.size;
        // The same physical page through C, for comparison.
        if (probe >= kStarts[1] && probe < kStarts[2])
        {
            MemoryInfo alias{};
            if (R_SUCCEEDED(svcQueryMemory(&alias, &pageInfo, uintptr_t(s_base + probe - kStarts[1] + kStarts[2]))))
            {
                repair.aliasType = alias.type;
                repair.aliasPermission = alias.perm;
            }
        }
        // Where the chunk's view starts in the window and in its source.
        const uint64_t chunkStart = chunk * kChunk;
        const uint64_t mapStart = chunkStart == 0 ? kPage : chunkStart;
        const uint64_t chunkEnd = chunkStart + kChunk;
        const uintptr_t source = s_source[chunk];
        repair.source = source;
        if (source && s_backend == Backend::ProcessMemory)
        {
            MemoryInfo backing{};
            if (R_SUCCEEDED(svcQueryMemory(&backing, &pageInfo, source)))
            {
                repair.sourceType = backing.type;
                repair.sourcePermission = backing.perm;
            }
        }
        // The chunk's memory blocks as the kernel sees them now.
        size_t length = 0;
        for (uint64_t at = mapStart; at < chunkEnd && length + 48 < sizeof(repair.layout);)
        {
            MemoryInfo block{};
            if (R_FAILED(svcQueryMemory(&block, &pageInfo, uintptr_t(s_base + at))) || block.size == 0)
                break;
            const uint64_t blockEnd = std::min<uint64_t>(chunkEnd, uint64_t(block.addr + block.size - uintptr_t(s_base)));
            const int written = std::snprintf(repair.layout + length, sizeof(repair.layout) - length, "%s%#llx-%#llx t%x p%x",
                length ? ", " : "", (unsigned long long)at, (unsigned long long)blockEnd, block.type, block.perm);
            if (written <= 0)
                break;
            length += size_t(written);
            at = blockEnd;
        }
        // Map only the unmapped block, within this chunk, onto the matching part of the source.
        const uint64_t blockStart = std::max<uint64_t>(mapStart, uint64_t(info.addr - uintptr_t(s_base)));
        const uint64_t blockEnd = std::min<uint64_t>(chunkEnd, uint64_t(info.addr + info.size - uintptr_t(s_base)));
        repair.mappedStart = blockStart;
        repair.mappedSize = blockEnd > blockStart ? blockEnd - blockStart : 0;
        uint32_t repairResult = 0xFFFFFFFFu;
        if (source && blockEnd > blockStart)
        {
            if (s_backend == Backend::TransferMemory)
            {
                // Transfer memory maps from its first page only.
                if (blockStart == mapStart)
                    repairResult = uint32_t(svcMapTransferMemory(Handle(source), s_base + blockStart,
                        blockEnd - blockStart, Perm_Rw));
            }
            else
            {
                repairResult = uint32_t(svcMapProcessMemory(s_base + blockStart, s_process,
                    source + (blockStart - mapStart), blockEnd - blockStart));
            }
        }
        repair.result = repairResult;
        repair.count = s_repair.count + 1;
        s_repair = repair;
        if (R_FAILED(repair.result))
            return Backing::Failed;
        result = Backing::Repaired;
    }
    return result;
}

RepairInfo LastRepair()
{
    std::lock_guard lock(s_mutex);
    return s_repair;
}

size_t CommittedBytes()
{
    std::lock_guard lock(s_mutex);
    return s_committedBytes;
}

const char* DiagnosticsText()
{
    return s_diagnostics;
}
}
#endif

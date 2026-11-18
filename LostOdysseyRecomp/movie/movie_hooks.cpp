// Native cutscene video. The game's WMV player (the Xbox Windows Media SDK,
// run as recompiled guest code) decodes 720p WMV3 at about real time on the
// Switch, and its GetNextFrame blocks the render thread until a frame is
// ready, so cutscenes stutter and the whole game stalls when it falls behind.
// With this on, the video is also decoded on a host thread with FFmpeg's VC-1
// decoder and copied into the player's Y/U/V textures, and the guest player
// is asked never to wait: GetNextFrame's "no wait" flag (descriptor +4 bit 0,
// read back through the player's vtable +252) makes it return "no new frame"
// instead of waiting for a sample or sleeping until a frame is due. The guest
// player is otherwise untouched: it still plays the audio, keeps the movie
// state and reports the end of the movie.
//
// Guest functions involved:
//   sub_82815060(M)        create the player for movie M (M+20 player, M+44 info)
//   sub_823F0FA8(pl, d)    next frame into descriptor d (render thread)
//   sub_82CE9248 / sub_82CE9FD8(pl)  pause / resume
//   sub_82CF8140(pl)       the player's video pump thread (player vtable +144,
//                          thread player+268): runs the guest WMV decoder
// On by default on the Switch, where the guest decoder freezes cutscenes
// (LO_NATIVE_MOVIES=0 turns it off); elsewhere LO_NATIVE_MOVIES=1 opts in.
#include <stdafx.h>
#include "movie_player.h"

#if LO_HAS_WMV3_DECODER

#include <os/logger.h>
#include <os/thread_name.h>
#include <kernel/memory.h>
#include <kernel/io/file_system.h>
#include <host_ui/host_ui.h>
#include <modding/fpi_index.h>
#include "wmv_stream.h"
#if LO_PLATFORM_SWITCH
#include <os/switch_platform.h>
#endif
#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>

extern "C" PPC_FUNC(__imp__sub_82815060);
extern "C" PPC_FUNC(__imp__sub_823F0FA8);
extern "C" PPC_FUNC(__imp__sub_82CE9248);
extern "C" PPC_FUNC(__imp__sub_82CE9FD8);
extern "C" PPC_FUNC(__imp__sub_82CF8140);
extern "C" PPC_FUNC(__imp__sub_82815208);
extern "C" PPC_FUNC(__imp__sub_82813A50);
extern "C" PPC_FUNC(__imp__sub_827E2DD0);
extern "C" PPC_FUNC(__imp__sub_827DDAA0);
extern "C" PPC_FUNC(__imp__sub_82578048);

namespace
{
namespace fs = std::filesystem;

// GetNextFrame results the movie update (sub_823F0038) checks for.
constexpr uint32_t kEndOfMovie = 0x16660026;
constexpr uint32_t kNoNewFrame = 0x166600E8;

bool Enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("LO_NATIVE_MOVIES");
        if (!value || !value[0])
            return bool(LO_PLATFORM_SWITCH);
        return std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

std::string Lower(std::string text)
{
    for (auto& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// One movie at a time: the game plays a single cutscene and starts each one
// through sub_82815060, which replaces the previous one here.
struct Movie
{
    std::unique_ptr<movie::WmvStream> stream;
    std::string name;
    uint32_t object = 0;      // guest movie object M (diagnostics)
    uint32_t player = 0;      // guest player object (M+20)
    bool started = false;     // first frame requested
    uint64_t startNs = 0;     // game time of the first request
    uint64_t pausedNs = 0, pauseStartNs = 0;
    bool paused = false;
    uint32_t shown = 0, late = 0;
    uint32_t lastPts = 0;
    bool loggedEnd = false;
    bool failed = false;
    // Diagnostics (2026-11-18: mv_01v never showed a frame after New Game).
    std::chrono::steady_clock::time_point openedAt = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastStatusAt{};
    uint32_t calls = 0, noNew = 0, guestWrote = 0, errors = 0;
    uint32_t lastResult = 0, lastTimeMs = 0;
    bool blanked = false;     // Y/U/V textures set to black before the first frame
#if LO_PLATFORM_SWITCH
    uint32_t guestVideoThread = 0;
    int guestVideoPriority = -1;
#endif
};

std::mutex g_mutex;
std::unique_ptr<Movie> g_movie;
// Diagnostics: calls of the movie stream job (sub_82815208) and stream read requests (sub_82813A50).
std::atomic<uint32_t> g_jobTicks{0}, g_streamRequests{0};
// Diagnostics: event timeline movie track ticks (sub_827E2DD0) and the last time passed to it.
std::atomic<uint32_t> g_trackTicks{0}, g_prepareCalls{0};
std::atomic<float> g_trackTime{0.f};

// Archive and offset of a movie file on the current disc, from its LO.fpi.
struct Location
{
    std::string archive;
    uint64_t offset = 0;
    uint64_t size = 0;
};

bool Locate(const std::string& name, Location& location, std::string& error)
{
    const fs::path index = FileSystem::ResolvePath("\\Device\\Cdrom0\\LO.fpi");
    if (index.empty())
    {
        error = "no disc";
        return false;
    }
    // Parsed once per disc.
    static std::mutex cacheMutex;
    std::lock_guard cacheLock(cacheMutex);
    static std::map<std::string, modding::fpi::Index> cache;
    const std::string key = FileSystem::PathUtf8(index);
    auto it = cache.find(key);
    if (it == cache.end())
    {
        std::ifstream input(index, std::ios::binary);
        std::vector<uint8_t> bytes(64);
        if (!input || !input.read(reinterpret_cast<char*>(bytes.data()), 64))
        {
            error = "cannot read " + key;
            return false;
        }
        try
        {
            const auto used = modding::fpi::IndexSize(bytes);
            bytes.resize(size_t(used));
            if (!input.read(reinterpret_cast<char*>(bytes.data()) + 64, std::streamsize(used - 64)))
            {
                error = "short read of " + key;
                return false;
            }
            it = cache.emplace(key, modding::fpi::Read(bytes)).first;
        }
        catch (const std::exception& e)
        {
            error = std::string("bad index: ") + e.what();
            return false;
        }
    }
    const auto& parsed = it->second;
    // The game names movies by file name ("mv_00v.wmv"); the index holds
    // "bin/xenon/mov/mv_00v.wmv".
    const std::string suffix = "/" + name;
    const modding::fpi::File* found = nullptr;
    for (const auto& file : parsed.files)
    {
        if (file.path == name || (file.path.size() > suffix.size() &&
                                  file.path.compare(file.path.size() - suffix.size(), suffix.size(), suffix) == 0))
        {
            // Prefer the movie folder over test data with the same name.
            if (!found || file.path.find("/mov/") != std::string::npos) found = &file;
        }
    }
    if (!found)
    {
        error = name + " is not in the disc index";
        return false;
    }
    if (found->archive >= parsed.archives.size())
    {
        error = "bad archive number";
        return false;
    }
    // Archive names in the index are lower case; match the disc files case-insensitively.
    const std::string archiveName = parsed.archives[found->archive];
    fs::path archive;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(index.parent_path(), ec))
    {
        if (Lower(FileSystem::PathUtf8(entry.path().filename())) == archiveName)
        {
            archive = entry.path();
            break;
        }
    }
    if (archive.empty())
    {
        error = archiveName + " missing";
        return false;
    }
    location.archive = FileSystem::PathUtf8(archive);
    location.offset = found->offset;
    location.size = found->size;
    return true;
}

// The movie's name: an FString (UTF-16 big endian) at info+48.
std::string MovieName(uint8_t* base, uint32_t info)
{
    const uint32_t data = PPC_LOAD_U32(info + 48);
    const int32_t count = int32_t(PPC_LOAD_U32(info + 52));
    std::string name;
    if (!data || count <= 0 || count > 260) return name;
    for (int32_t i = 0; i < count; ++i)
    {
        const uint16_t c = PPC_LOAD_U16(data + uint32_t(i) * 2);
        if (!c) break;
        if (c >= 0x80) return {};
        name += char(c == '\\' ? '/' : std::tolower(c));
    }
    if (const auto slash = name.rfind('/'); slash != std::string::npos) name.erase(0, slash + 1);
    return name;
}

void DecoderThreadSetup(const char* name)
{
    os::SetCurrentThreadName(name);
#if LO_PLATFORM_SWITCH
    // Free to run on any application core (core 2 preferred); new threads
    // otherwise stay on the creating core.
    os::switch_platform::SetThreadIdealCore(0, 2);
#endif
}

uint64_t MovieTimeNs(Movie& movie, uint64_t now)
{
    uint64_t paused = movie.pausedNs;
    if (movie.paused) paused += now - movie.pauseStartNs;
    const uint64_t elapsed = now - movie.startNs;
    return elapsed > paused ? elapsed - paused : 0;
}

void LogEnd(Movie& movie, const char* why)
{
    if (movie.loggedEnd || !movie.stream) return;
    movie.loggedEnd = true;
    const uint32_t decoded = movie.stream->DecodedFrames();
    const double seconds = movie.stream->DecodeSeconds();
    LOG_NOTICE("native movie: {} {}: {} frames decoded ({:.1f} fps decode speed), {} shown, {} late{}{}", movie.name, why,
               decoded, seconds > 0 ? decoded / seconds : 0.0, movie.shown, movie.late,
               movie.stream->LastError().empty() ? "" : ", error: ", movie.stream->LastError());
}

// Caller holds g_mutex.
void LogStatus(Movie& movie, const char* why)
{
    size_t queued = 0;
    uint32_t frontPts = 0;
    bool ended = false;
    if (movie.stream) movie.stream->Status(queued, frontPts, ended);
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - movie.openedAt);
    LOG_WARNING("native movie: {} status ({}): {} ms since start, GetNextFrame calls {} (no new {}, guest wrote {}, "
                "errors {}, last result {:#x}), movie time {} ms, shown {}, host decoded {}, queued {} (first pts {}), "
                "decoder ended {}, error '{}'",
                movie.name, why, age.count(), movie.calls, movie.noNew, movie.guestWrote, movie.errors, movie.lastResult,
                movie.lastTimeMs, movie.shown, movie.stream ? movie.stream->DecodedFrames() : 0, queued, frontPts, ended,
                movie.stream ? movie.stream->LastError() : std::string());
    // The movie object's stream state (see sub_828143C8 / sub_82813A50): M+104 player state (2 = created), M+108
    // stream step 0-3, info (M+44) +20 load status (2 = done), +22 request (1 = pending, 255 = released),
    // +24 destination, +28 size, +36 file size, +40 offset; caches M+48 / M+76: +12 buffer, +20 loaded, +24 end.
    const uint32_t m = movie.object;
    auto r32 = [](uint32_t a) -> uint32_t {
        return g_memory.base && a >= 0x10000 && a < 0xFFFFFFF0u ? __builtin_bswap32(*reinterpret_cast<const uint32_t*>(g_memory.base + a)) : 0;
    };
    auto r16 = [](uint32_t a) -> uint32_t {
        return g_memory.base && a >= 0x10000 && a < 0xFFFFFFF0u ? __builtin_bswap16(*reinterpret_cast<const uint16_t*>(g_memory.base + a)) : 0;
    };
    if (!m) return;
    const uint32_t info = r32(m + 44);
    LOG_WARNING("native movie: {} object M={:#x} flags8={:#x} state104={} step108={} info={:#x} status20={} request22={} "
                "dest24={:#x} size28={:#x} file36={:#x} offset40={:#x} cache48 buf={:#x} loaded={:#x} end={:#x} "
                "cache76 buf={:#x} loaded={:#x} end={:#x} jobTicks={} requests={} trackTicks={} trackTime={:.3f}",
                movie.name, m, r32(m + 8) >> 24, int16_t(r16(m + 104)), r32(m + 108), info, info ? r16(info + 20) : 0,
                info ? r16(info + 22) : 0, info ? r32(info + 24) : 0, info ? r32(info + 28) : 0, info ? r32(info + 36) : 0,
                info ? r32(info + 44) : 0, r32(m + 48 + 12), r32(m + 48 + 20), r32(m + 48 + 24), r32(m + 76 + 12),
                r32(m + 76 + 20), r32(m + 76 + 24), g_jobTicks.load(), g_streamRequests.load(), g_trackTicks.load(),
                g_trackTime.load());
    std::string words;
    for (uint32_t i = 0; i < 128; i += 4) words += fmt::format(" {:08x}", r32(m + i));
    LOG_WARNING("native movie: M+0..127:{}", words);
    words.clear();
    for (uint32_t i = 0; i < 68 && info; i += 4) words += fmt::format(" {:08x}", r32(info + i));
    LOG_WARNING("native movie: info+0..67:{}", words);
}
}

// Movie start: open the video on the host as well.
PPC_FUNC(sub_82815060)
{
    const uint32_t m = ctx.r3.u32;
    std::unique_ptr<Movie> previous;
    {
        std::lock_guard lock(g_mutex);
        previous = std::move(g_movie);
    }
    if (previous)
    {
        LogEnd(*previous, "replaced");
        previous.reset(); // stops its decoder thread
    }
    if (!Enabled())
    {
        __imp__sub_82815060(ctx, base);
        return;
    }

    auto movie = std::make_unique<Movie>();
    const uint32_t info = PPC_LOAD_U32(m + 44);
    const int32_t videoTrack = info ? int32_t(PPC_LOAD_U32(info + 64)) : -1;
    movie->name = info ? MovieName(base, info) : std::string();
    std::string error;
    Location location;
    if (movie->name.empty())
        error = "no movie name";
    else if (videoTrack == -1)
        error = "movie has no video track";
    else if (Locate(movie->name, location, error))
    {
        movie->stream = std::make_unique<movie::WmvStream>();
        if (!movie->stream->Open(location.archive, location.offset, location.size, videoTrack, error))
            movie->stream.reset();
    }
    if (!movie->stream)
    {
        LOG_WARNING("native movie: {} uses the guest decoder ({})", movie->name.empty() ? "?" : movie->name, error);
        __imp__sub_82815060(ctx, base);
        return;
    }

    __imp__sub_82815060(ctx, base);

    const uint32_t player = PPC_LOAD_U32(m + 20);
    const uint32_t width = PPC_LOAD_U32(m + 120), height = PPC_LOAD_U32(m + 124);
    if (!player || width != movie->stream->Width() || height != movie->stream->Height())
    {
        LOG_WARNING("native movie: {} uses the guest decoder (player {:#x}, textures {}x{})", movie->name, player, width,
                    height);
        return;
    }
    movie->player = player;
    movie->object = m;
    movie->stream->Start(DecoderThreadSetup);
    LOG_NOTICE("native movie: {} {}x{} video track {} at {}+{:#x}, player {:#x}", movie->name, movie->stream->Width(),
               movie->stream->Height(), videoTrack, location.archive, location.offset, player);
    std::lock_guard lock(g_mutex);
    g_movie = std::move(movie);
}

// Next frame (render thread): let the guest player run without waiting, then
// copy the host frame due now into the locked Y/U/V textures.
PPC_FUNC(sub_823F0FA8)
{
    const uint32_t player = ctx.r3.u32, desc = ctx.r4.u32;
    {
        std::lock_guard lock(g_mutex);
        if (g_movie && g_movie->player == player && !g_movie->failed && !g_movie->stream->LastError().empty())
        {
            g_movie->failed = true;
#if LO_PLATFORM_SWITCH
            if (g_movie->guestVideoThread && g_movie->guestVideoPriority >= 0)
                os::switch_platform::SetThreadPriority(g_movie->guestVideoThread, g_movie->guestVideoPriority);
#endif
            LogEnd(*g_movie, "failed");
        }
        if (!g_movie || !player || g_movie->player != player || g_movie->failed)
        {
            // Not ours: stock behaviour.
            ctx.r3.u64 = player;
            ctx.r4.u64 = desc;
            goto guest;
        }
    }
    {
        const uint32_t flags = PPC_LOAD_U32(desc + 4);
        PPC_STORE_U32(desc + 4, flags | 1); // never wait for this request
        __imp__sub_823F0FA8(ctx, base);
        PPC_STORE_U32(desc + 4, flags);
        const uint32_t result = ctx.r3.u32;
        PPC_STORE_U32(desc + 8, 0); // never "late": the caller would loop
        {
            std::lock_guard lock(g_mutex);
            if (g_movie && g_movie->player == player)
            {
                Movie& m = *g_movie;
                ++m.calls;
                m.lastResult = result;
                if (int32_t(result) < 0) ++m.errors;
                else if (result == kNoNewFrame) ++m.noNew;
                else if (result != kEndOfMovie) ++m.guestWrote;
                if (m.calls <= 3)
                    LOG_NOTICE("native movie: {} GetNextFrame #{} result {:#x}", m.name, m.calls, result);
                // Every 2 s until the first frame is shown.
                const auto now = std::chrono::steady_clock::now();
                if (m.shown == 0 && now - m.lastStatusAt >= std::chrono::seconds(2))
                {
                    m.lastStatusAt = now;
                    LogStatus(m, "no frame yet");
                }
            }
        }
        if (int32_t(result) < 0 || result == kEndOfMovie)
        {
            if (result == kEndOfMovie)
            {
                std::lock_guard lock(g_mutex);
                if (g_movie && g_movie->player == player) LogEnd(*g_movie, "finished");
            }
            return; // errors and the end of the movie come from the guest player
        }

        std::lock_guard lock(g_mutex);
        Movie* movie = g_movie.get();
        if (!movie || movie->player != player) return;
        const uint64_t now = host_ui::GetActiveGameTimeNs();
        if (!movie->started)
        {
            movie->started = true;
            movie->startNs = now;
        }
        const uint32_t timeMs = uint32_t(std::min<uint64_t>(MovieTimeNs(*movie, now) / 1000000, 0xFFFFFFFFu));
        movie->lastTimeMs = timeMs;
        bool fresh = false;
        const movie::VideoFrame* frame = movie->stream->FrameAt(timeMs, fresh);
        // A guest frame the player just wrote (result 0) is replaced by the
        // host's current frame; without a host frame nothing is shown new.
        const bool guestWrote = result != kNoNewFrame;
        const uint32_t y = PPC_LOAD_U32(desc + 12), u = PPC_LOAD_U32(desc + 16), v = PPC_LOAD_U32(desc + 20);
        const uint32_t yPitch = PPC_LOAD_U32(desc + 36), uPitch = PPC_LOAD_U32(desc + 40), vPitch = PPC_LOAD_U32(desc + 44);
        if (!frame || (!fresh && !guestWrote))
        {
            // Until the host's first frame the guest draws the player's textures as they are: zeroed memory, which
            // YUV turns into green (Switch 2, the green flash between the loading screen and mv_01v, 2026-11-18).
            // Make them black once: Y 0, U and V 128.
            const uint32_t bw = movie->stream->Width(), bh = movie->stream->Height();
            if (!movie->blanked && movie->shown == 0 && y && u && v && bw && bh && yPitch >= bw && uPitch >= bw / 2 &&
                vPitch >= bw / 2)
            {
                movie->blanked = true;
                for (uint32_t row = 0; row < bh; ++row)
                    std::memset(base + y + size_t(row) * yPitch, 0, bw);
                for (uint32_t row = 0; row < bh / 2; ++row)
                {
                    std::memset(base + u + size_t(row) * uPitch, 128, bw / 2);
                    std::memset(base + v + size_t(row) * vPitch, 128, bw / 2);
                }
                ctx.r3.u64 = 0; // a new (black) picture: the guest uploads it
                return;
            }
            ctx.r3.u64 = kNoNewFrame;
            return;
        }
        const uint32_t w = frame->width, h = frame->height;
        if (!y || !u || !v || yPitch < w || uPitch < w / 2 || vPitch < w / 2)
        {
            ctx.r3.u64 = kNoNewFrame;
            return;
        }
        for (uint32_t row = 0; row < h; ++row)
            std::memcpy(base + y + size_t(row) * yPitch, frame->y.data() + size_t(row) * w, w);
        for (uint32_t row = 0; row < h / 2; ++row)
        {
            std::memcpy(base + u + size_t(row) * uPitch, frame->u.data() + size_t(row) * (w / 2), w / 2);
            std::memcpy(base + v + size_t(row) * vPitch, frame->v.data() + size_t(row) * (w / 2), w / 2);
        }
        if (fresh)
        {
            // A frame more than two frame times old: the decoder is behind.
            if (timeMs > frame->ptsMs + 67) ++movie->late;
            if (movie->shown++ == 0)
                LOG_NOTICE("native movie: {} first frame at {} ms", movie->name, timeMs);
            movie->lastPts = frame->ptsMs;
        }
        ctx.r3.u64 = 0;
        return;
    }
guest:
    __imp__sub_823F0FA8(ctx, base);
}

PPC_FUNC(sub_82CE9248)
{
    const uint32_t player = ctx.r3.u32;
    __imp__sub_82CE9248(ctx, base);
    std::lock_guard lock(g_mutex);
    if (g_movie && g_movie->player == player && g_movie->started && !g_movie->paused)
    {
        g_movie->paused = true;
        g_movie->pauseStartNs = host_ui::GetActiveGameTimeNs();
    }
}

PPC_FUNC(sub_82CE9FD8)
{
    const uint32_t player = ctx.r3.u32;
    __imp__sub_82CE9FD8(ctx, base);
    std::lock_guard lock(g_mutex);
    if (g_movie && g_movie->player == player && g_movie->paused)
    {
        g_movie->paused = false;
        g_movie->pausedNs += host_ui::GetActiveGameTimeNs() - g_movie->pauseStartNs;
    }
}

// The guest video pump keeps decoding (about one core) although its frames
// are replaced. LO_MOVIE_GUEST_VIDEO_PRIORITY=<hex 2C-3F> runs it at a lower
// priority, so it only gets CPU time the audio, the game and the host decoder
// leave over. Off by default (2026-11-10): the pump is not idle work. It feeds
// the player's reader, so the movie's audio stops when it is starved, and
// closing the movie waits for it to stop. When it is lowered, closing or
// releasing the movie gives it back the game's priority first
// (RestoreGuestVideoPriority). The black screen after the Gongora coronation
// video was the pump stuck in the read callback during the close, at any
// priority; see sub_82813948 in gpu/movie_replace.cpp.
PPC_FUNC(sub_82CF8140)
{
    bool lower = false;
    {
        std::lock_guard lock(g_mutex);
        lower = g_movie && ctx.r3.u32 && g_movie->player == ctx.r3.u32;
    }
#if LO_PLATFORM_SWITCH
    static const int priority = [] {
        const char* value = std::getenv("LO_MOVIE_GUEST_VIDEO_PRIORITY");
        return value && *value ? int(std::strtol(value, nullptr, 16)) : 0;
    }();
    if (lower && priority >= 0x2C && priority <= 0x3F)
    {
        std::lock_guard lock(g_mutex);
        if (g_movie && !g_movie->failed && g_movie->player == ctx.r3.u32)
        {
            const int originalPriority = os::switch_platform::CurrentThreadPriority();
            if (originalPriority >= 0)
            {
                g_movie->guestVideoThread = os::switch_platform::CurrentThreadHandle();
                g_movie->guestVideoPriority = originalPriority;
                os::switch_platform::SetCurrentThreadPriority(priority);
                LOG_NOTICE("native movie: guest video decoder thread priority {:#x}", priority);
            }
        }
    }
#else
    (void)lower;
#endif
    __imp__sub_82CF8140(ctx, base);
}

// Diagnostics (2026-11-18, mv_01v stuck after New Game): the movie's stream job, run by the game's job threads.
// It returns at once while M+104 < 2 (player not created), else steps the stream state machine (sub_828143C8).
PPC_FUNC(sub_82815208)
{
    const uint32_t m = ctx.r3.u32;
    const uint32_t ticks = ++g_jobTicks;
    if (ticks <= 3)
        LOG_NOTICE("native movie: stream job #{} M={:#x} state104={} step108={}", ticks, m,
                   int16_t(PPC_LOAD_U16(m + 104)), PPC_LOAD_U32(m + 108));
    __imp__sub_82815208(ctx, base);
}

// Stream read request: r3 = M, r4 = cache (M+48 / M+76), r5 = 0 issue / 1 collect. Returns 1 when issued/collected.
PPC_FUNC(sub_82813A50)
{
    const uint32_t m = ctx.r3.u32, cache = ctx.r4.u32, mode = ctx.r5.u32;
    __imp__sub_82813A50(ctx, base);
    const uint32_t n = ++g_streamRequests;
    if (n <= 12)
    {
        const uint32_t info = PPC_LOAD_U32(m + 44);
        LOG_NOTICE("native movie: stream request #{} M={:#x} cache=+{} mode={} -> {} (loaded {:#x} of {:#x}, status20={} request22={})",
                   n, m, cache - m, mode, ctx.r3.u32, PPC_LOAD_U32(cache + 20), info ? PPC_LOAD_U32(info + 36) : 0,
                   info ? PPC_LOAD_U16(info + 20) : 0, info ? PPC_LOAD_U16(info + 22) : 0);
    }
}

// Diagnostics: the event timeline's movie track tick. r3 = track (+108 entries of 20 bytes: preload, start,
// length floats ...; +112 entry count), r5 = owner (+60 previous time, +64 movie M), f1 = current time.
// It creates and starts M when the time passes an entry's preload time, moves M+104 from 2 (player created)
// to 3 and tells the owner (vtable +280), and starts playback once the time reaches the entry's start.
PPC_FUNC(sub_827E2DD0)
{
    const uint32_t track = ctx.r3.u32, owner = ctx.r5.u32;
    const double now = ctx.f1.f64;
    const uint32_t lr = uint32_t(ctx.lr);
    auto f32 = [&](uint32_t a) { return std::bit_cast<float>(PPC_LOAD_U32(a)); };
    const uint32_t n = ++g_trackTicks;
    g_trackTime.store(float(now));
    const uint32_t entries = PPC_LOAD_U32(track + 108), count = PPC_LOAD_U32(track + 112);
    const uint32_t m = PPC_LOAD_U32(owner + 64);
    if (n <= 8 || (n <= 6000 && n % 120 == 0))
        LOG_NOTICE("native movie: track tick #{} track={:#x} owner={:#x} lr={:#x} time={:.3f} prev={:.3f} entries={} "
                   "first preload={:.3f} start={:.3f} length={:.3f} M={:#x} state104={}",
                   n, track, owner, lr, now, f32(owner + 60), count, count ? f32(entries) : 0.f,
                   count ? f32(entries + 4) : 0.f, count ? f32(entries + 8) : 0.f, m, m ? int16_t(PPC_LOAD_U16(m + 104)) : -1);
    __imp__sub_827E2DD0(ctx, base);
}

// Diagnostics: the movie track's "prepare" step (track vtable +356). r3 = track, r5 = owner, f1 = time. It runs
// the owner's +272 and the track tick (+312, sub_827E2DD0) and returns 1 when the track is ready: no movie, or
// M+104 == 3. 2026-11-18: called 3-4 times right after the movie start, then never again, so M+104 stays 2.
// Logs the first 12 calls with the guest caller chain, so the loop that stops calling it can be found.
PPC_FUNC(sub_827DDAA0)
{
    const uint32_t track = ctx.r3.u32, owner = ctx.r5.u32, lr = uint32_t(ctx.lr);
    const double now = ctx.f1.f64;
    std::string chain;
    for (uint32_t frame = ctx.r1.u32, i = 0; i < 12 && frame >= 0x1000 && frame < 0x7C000000; ++i)
    {
        const uint32_t caller = PPC_LOAD_U32(frame);
        if (caller <= frame || caller >= 0x7C000000) break;
        const uint32_t ret = PPC_LOAD_U32(caller - 8);
        if (ret < 0x82000000 || ret >= 0x83400000) break;
        chain += fmt::format(" {:#x}", ret);
        frame = caller;
    }
    __imp__sub_827DDAA0(ctx, base);
    const uint32_t n = ++g_prepareCalls;
    if (n <= 12)
    {
        const uint32_t m = PPC_LOAD_U32(owner + 64);
        LOG_NOTICE("native movie: track prepare #{} track={:#x} owner={:#x} time={:.3f} -> {} M={:#x} state104={} lr={:#x} "
                   "callers:{}",
                   n, track, owner, now, ctx.r3.u32 & 0xFF, m, m ? int16_t(PPC_LOAD_U16(m + 104)) : -1, lr, chain);
    }
}

// Diagnostics: an event group's prepare (sub_82578048, shared by several group classes). r3 = group (+64 track
// list, +68 count), r5 = owner (+68 per-track owner list, +72 count), f1 = time. Calls each track's prepare
// (vtable +356) in order and returns 0 at the first one not ready, 1 when all are (or the counts differ).
// Re-implemented here call for call (same order, same registers, same lr) so the track that keeps the event
// from starting can be named. Logged when a group's result or blocking track changes (up to 300 lines).
PPC_FUNC(sub_82578048)
{
    static const bool trace = [] {
        const char* value = std::getenv("LO_EVENT_PREPARE_TRACE");
        return !value || !*value || *value != '0';
    }();
    if (!trace)
    {
        __imp__sub_82578048(ctx, base);
        return;
    }
    const uint32_t group = ctx.r3.u32, owner = ctx.r5.u32, lr = uint32_t(ctx.lr);
    const double now = ctx.f1.f64;
    const int32_t count = int32_t(PPC_LOAD_U32(group + 68));
    uint32_t result = 1, blocking = 0, blockingIndex = 0;
    if (count == int32_t(PPC_LOAD_U32(owner + 72)) && count > 0)
    {
        const uint32_t savedR1 = ctx.r1.u32;
        ctx.r1.u32 = savedR1 - 128; // the frame the original allocates
        for (int32_t i = 0; i < count; ++i)
        {
            const uint32_t track = PPC_LOAD_U32(PPC_LOAD_U32(group + 64) + uint32_t(i) * 4);
            ctx.r3.u64 = track;
            ctx.r5.u64 = PPC_LOAD_U32(PPC_LOAD_U32(owner + 68) + uint32_t(i) * 4);
            ctx.f1.f64 = now;
            const uint32_t fn = PPC_LOAD_U32(PPC_LOAD_U32(track) + 356);
            ctx.lr = 0x825780A8;
            g_memory.FindFunction(fn)(ctx, base);
            if ((ctx.r3.u32 & 0xFF) == 0)
            {
                result = 0;
                blocking = track;
                blockingIndex = uint32_t(i);
                break;
            }
        }
        ctx.r1.u32 = savedR1;
    }
    ctx.r3.u64 = result;
    ctx.lr = lr;

    static std::mutex logMutex;
    static std::map<uint32_t, std::pair<uint32_t, uint32_t>> last; // group -> (result, blocking track)
    static uint32_t lines = 0, calls = 0;
    std::lock_guard lock(logMutex);
    ++calls;
    auto& state = last[group];
    const std::pair<uint32_t, uint32_t> current{result + 1, blocking}; // result + 1: a new group always logs
    if (state != current && lines < 300)
    {
        ++lines;
        const uint32_t vtable = blocking ? PPC_LOAD_U32(blocking) : 0;
        LOG_NOTICE("event group prepare: group={:#x} owner={:#x} lr={:#x} time={:.3f} tracks={} -> {}{} (call {})", group,
                   owner, lr, now, count, result,
                   blocking ? fmt::format(", blocked by track #{} {:#x} vtable {:#x} prepare {:#x}", blockingIndex, blocking,
                                          vtable, vtable ? PPC_LOAD_U32(vtable + 356) : 0)
                            : std::string(),
                   calls);
    }
    state = current;
    if (calls % 600 == 0 && lines < 300)
    {
        ++lines;
        LOG_NOTICE("event group prepare: {} calls, {} groups seen", calls, last.size());
    }
}

bool movie::NativeMoviesEnabled()
{
    return Enabled();
}

uint32_t movie::NativeMovieStallMs()
{
    std::lock_guard lock(g_mutex);
    if (!g_movie || g_movie->shown || g_movie->loggedEnd) return 0;
    const auto age = std::chrono::steady_clock::now() - g_movie->openedAt;
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(age).count());
}

void movie::LogNativeMovieState()
{
    std::lock_guard lock(g_mutex);
    if (g_movie) LogStatus(*g_movie, "watchdog");
}

void movie::RestoreGuestVideoPriority(uint32_t player)
{
#if LO_PLATFORM_SWITCH
    std::lock_guard lock(g_mutex);
    if (!player || !g_movie || g_movie->player != player || !g_movie->guestVideoThread || g_movie->guestVideoPriority < 0)
        return;
    os::switch_platform::SetThreadPriority(g_movie->guestVideoThread, g_movie->guestVideoPriority);
    LOG_NOTICE("native movie: {} closing, guest video decoder thread back at priority {:#x}", g_movie->name,
               g_movie->guestVideoPriority);
    g_movie->guestVideoPriority = -1;
#else
    (void)player;
#endif
}

#else

bool movie::NativeMoviesEnabled()
{
    return false;
}

uint32_t movie::NativeMovieStallMs()
{
    return 0;
}

void movie::LogNativeMovieState()
{
}

void movie::RestoreGuestVideoPriority(uint32_t)
{
}

#endif

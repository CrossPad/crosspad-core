// SPDX-License-Identifier: GPL-3.0-or-later
//
// SongEngine's private implementation: the state the render, the worker and the
// UI share, declared once so the engine can live in several translation units.
// SongEngine.cpp has the worker loop and the public forwarders; the other
// SongEngine*.cpp files hold the methods of one area each (projects, edits,
// freeze/bounce, playback, takes). Not a public header: nothing outside
// src/song/ includes it.

#pragma once

#include "crosspad/song/SongEngine.hpp"

#include "crosspad/audio/WavWriter.hpp"
#include "crosspad/dsp/TimeStretch.hpp"
#include "crosspad/instrument/WavPcm.hpp"
#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/kit/KitInfo.hpp"
#include "crosspad/platform/IFileSystem.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sequencer/MetronomeNode.hpp"
#include "crosspad/sequencer/PatternSequencer.hpp"
#include "crosspad/song/BakedView.hpp"
#include "crosspad/song/ElementResolver.hpp"
#include "crosspad/song/LiveSet.hpp"
#include "crosspad/song/SeqFreezeKey.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace crosspad {

namespace songdetail {

constexpr int      kTracks           = AudioSong::kTracks;
constexpr uint32_t kBounceMinFrames  = 8192;   // 32 kB (~170 ms), the least a bounce starts with
static_assert(WavWriter::kHeaderBytes % 4 == 0, "the WAV header is whole frames");
constexpr uint32_t kPrimeFrames      = 4096;
/// A varispeed read's source frames beyond a chunk: the second frame each
/// interpolation takes, and the frame the next chunk goes on from.
constexpr uint32_t kVarispeedGuard   = 2;
/// The tracks' meters: the loudest frame of each track in every hop of the
/// ring, kept for long enough that what RT is playing is still there.
constexpr uint32_t kMeterHop         = 1024;   // ~23 ms
constexpr uint32_t kMeterSlots       = 32;     // twice the ring
constexpr uint32_t kMeterRead        = 3;      // hops a read looks back: a UI tick's worth
/// A project name, as a folder on the card and a row in a list.
constexpr size_t   kMaxNameLen       = 32;
/// The kit's part of a clip's file name.
constexpr size_t   kMaxClipPrefix    = 32;
/// While the card is not there yet, how often the worker looks for it.
constexpr uint32_t kCardPollMs       = 1000;
/// How often the worker tops the song ring up while it plays.
constexpr uint32_t kStreamPollMs     = 3;
/// A commit is saved once nothing has been committed for this long: a fader
/// turned is dozens of commits, and each one a card write under playback.
constexpr uint32_t kSaveDelayMs      = 400;
/// A playing track's read-ahead (ClipReader::setAhead()): four chunks of the
/// card at a time, in whole sectors.
constexpr uint32_t kAheadBytes       = 16384;
/// After a freeze gave way (yieldFreeze()), how long no new one starts.
constexpr int64_t  kYieldHoldUs      = 1500000;
/// Why a freeze stopped for a player on its engine (runBounce()).
constexpr const char* kGaveWay       = "waiting: the pads play its engine";
/// Undo: steps kept each way, and how close changes come to be one step.
constexpr size_t   kUndoDepth        = 24;
/// A take onto the seq track: notes the sequencer keeps between two worker
/// passes (it is emptied every few ms -- this is a stall's worth of hits),
/// and how far past its end the song runs while a take is open.
constexpr size_t   kTakeRoom         = 256;
constexpr uint32_t kTakeHorizonBars  = 999;
/// Patterns kept as samples for the sequencer: blocks sharing notes, and a
/// commit that changed nothing about them, convert none again.
constexpr size_t   kNoteCacheSize    = 32;
constexpr int64_t  kUndoMergeUs      = 700000;
/// requestTempo(): the tempi a song may have.
constexpr float    kMinBpm           = 20.0f;
constexpr float    kMaxBpm           = kMaxSongBpm;
/// releaseRings(): how often it logs that it is still waiting (a bounce
/// notices a cancel within one card write).
constexpr uint32_t kReleaseWaitMs    = 3000;
constexpr uint32_t kReleasePollMs    = 10;
/// A clip's waveform: one value per this many frames (23 ms at 44.1 kHz,
/// under a pixel at the Arrange's closest zoom).
constexpr uint32_t kPeakHop          = 1024;
/// Frames read from the card at a time while a waveform is worked out.
constexpr uint32_t kPeakChunkFrames  = 4096;
/// Beside clips/: <file>.pk per clip, so a waveform is read from its WAV once.
constexpr const char* kPeaksDir      = ".peaks";
/// A freeze's renders, in clips/, named by what they play (pieceKey()).
constexpr const char* kFreezeDir     = ".freeze";
/// How far a bake's mix has come when its last chunk is written (the rest is the rename).
constexpr uint8_t  kBakeMixedPercent = 99;
/// The cache's last-use counters, beside the bakes (bump_use()).
constexpr const char* kUsageFile     = "usage.json";
/// What a bake is written as before its rename; one left by an earlier run is cut short.
constexpr const char* kPartSuffix    = ".part";

/// A name that stays one folder under the songs root.
inline bool valid_name(const std::string& n)
{
    return !n.empty() && n.size() <= kMaxNameLen && n != "." && n != ".." &&
           n.find_first_of("/\\") == std::string::npos;
}

/// The few file operations the engine needs, on the platform's file system.
namespace fsx {
inline IFileSystem& fs() { return getFileSystem(); }
inline int  openRead(const std::string& p) { return fs().openRead(p.c_str()); }
inline long readSome(int fd, void* dst, size_t n) { return fs().read(fd, dst, n); }
inline bool seek(int fd, uint32_t at) { return fs().seek(fd, at); }
inline void close(int fd) { fs().close(fd); }
inline bool exists(const std::string& p) { return fs().exists(p.c_str()); }
/// @p p is a file, not a folder, and holds @p atLeast bytes.
inline bool isFile(const std::string& p, uint64_t atLeast = 0)
{
    FileStat st;
    return fs().stat(p.c_str(), &st) == FsStat::Found && !st.isDir && st.size >= atLeast;
}
/// @p p's size, and when it was last written in unix seconds.
inline bool fileInfo(const std::string& p, uint64_t& bytes, int64_t& modified)
{
    FileStat st;
    if (fs().stat(p.c_str(), &st) != FsStat::Found) return false;
    bytes = st.size;
    modified = st.modified;
    return true;
}
inline bool rename(const std::string& from, const std::string& to) { return fs().rename(from.c_str(), to.c_str()); }
inline bool makeDir(const std::string& p) { return fs().makeDir(p.c_str()); }
/// @p each(name, isDir) for every entry of @p dir.
template <typename F> void listDir(const std::string& dir, F each)
{
    fs().forEach(dir, [&](const char* name, bool isDir) { each(std::string(name), isDir); });
}
/// @p dir and everything in it, @p yield() after each entry.
template <typename Y> bool removeTree(const std::string& dir, Y yield)
{
    return fs().removeTree(dir, [](void* y) { (*static_cast<Y*>(y))(); }, &yield);
}
} // namespace fsx

/// Stereo int16 frames, one writer and one reader, counters that only grow
/// (so used() is right across the uint32 wrap). In 16 kB segments rather than
/// one block: on the board the largest free PSRAM block is often well under
/// 100 kB, and a ring that needed one contiguous 128 kB piece failed ("not
/// enough memory") with plenty free in total. The platform says where each
/// segment comes from.
struct FrameRing {
    ISongEnginePlatform* platform = nullptr;
    static constexpr uint32_t kSegFrames = 4096;       // 16 kB
    static constexpr uint32_t kMaxSegs   = 64;         // 1 MB: the largest ring a platform sizes
    int16_t* seg[kMaxSegs] = {};
    uint32_t segs = 0;
    uint32_t cap = 0;
    std::atomic<uint32_t> head{0}, tail{0};

    static constexpr size_t kSegBytes = kSegFrames * 4;

    /// As many segments as memory gives, up to @p frames, at least @p
    /// minFrames. A segment past the minimum is asked for with missingSegs
    /// 0, so a platform can keep more back for those.
    bool alloc(uint32_t frames, uint32_t minFrames) {
        release();
        const uint32_t want = std::min(kMaxSegs, (frames + kSegFrames - 1) / kSegFrames);
        const uint32_t need = (minFrames + kSegFrames - 1) / kSegFrames;
        while (segs < want) {
            void* p = platform->allocRingSegment(kSegBytes, segs < need ? need - segs : 0);
            if (!p) break;
            seg[segs++] = static_cast<int16_t*>(p);
        }
        cap = segs * kSegFrames;
        if (cap < minFrames) { release(); return false; }
        return true;
    }
    void release() {
        for (uint32_t i = 0; i < segs; ++i) platform->freeRingSegment(seg[i]);
        std::fill(std::begin(seg), std::end(seg), nullptr);
        segs = 0; cap = 0;
        head.store(0); tail.store(0);
    }
    uint32_t used()  const { return head.load(std::memory_order_acquire) - tail.load(std::memory_order_acquire); }
    uint32_t space() const { return cap - used(); }

    /// Copy @p n frames between @p ext and the ring from frame @p at on,
    /// segment by segment. @p ext null on a write means silence.
    void copy(uint32_t at, int16_t* ext, uint32_t n, bool toRing) {
        while (n) {
            const uint32_t pos = at % cap;
            int16_t* p = seg[pos / kSegFrames] + (pos % kSegFrames) * 2;
            const uint32_t run = std::min(n, kSegFrames - pos % kSegFrames);
            if (toRing) { if (ext) std::memcpy(p, ext, run * 4); else std::memset(p, 0, run * 4); }
            else        std::memcpy(ext, p, run * 4);
            if (ext) ext += run * 2;
            at += run;
            n -= run;
        }
    }

    uint32_t push(const int16_t* src, uint32_t n) {
        if (!cap) return 0;
        n = std::min(n, space());
        const uint32_t h = head.load(std::memory_order_relaxed);
        copy(h, const_cast<int16_t*>(src), n, true);
        head.store(h + n, std::memory_order_release);
        return n;
    }
    uint32_t pop(int16_t* dst, uint32_t n) {
        if (!cap) return 0;
        n = std::min(n, used());
        const uint32_t t = tail.load(std::memory_order_relaxed);
        copy(t, dst, n, false);
        tail.store(t + n, std::memory_order_release);
        return n;
    }
};

/// A clip being read: where its samples start and how it is laid out. 16-bit
/// PCM, mono or stereo; mono is played to both sides. A plain descriptor, not
/// a FILE: reads are whole chunks, and stdio's 4 kB buffer per open file was
/// 16 kB of PSRAM with four tracks playing.
struct ClipReader {
    ClipReader() = default;
    /* The descriptor is given back whatever happens. The card has a handful
     * of them and every one held open is one a pad's sample cannot have:
     * readPeaks() used to return without closing, and a project of thirty
     * clips -- the clip launcher asks for every waveform in view -- left the
     * card unable to open anything at all, opendir included (ENFILE). Not
     * copyable, so a copy cannot close another's. */
    ~ClipReader() { close(); }
    ClipReader(const ClipReader&) = delete;
    ClipReader& operator=(const ClipReader&) = delete;

    int         fd = -1;
    std::string path;
    uint32_t    dataOffset = 0;
    uint32_t    frames = 0;
    uint16_t    channels = 2;
    uint32_t    cursor = UINT32_MAX;   // frame the file position is at
    /* readVarispeed(): the last two frames of the file it read, by where they
     * are in it. A pass reads its next chunk on from them instead of seeking
     * back for the frame two chunks' interpolations share; a jump or another
     * pass is somewhere else in the file, and reads afresh. */
    uint32_t    heldAt = UINT32_MAX;
    int16_t     held[2 * kVarispeedGuard] = {};
    /* readWhole()'s read-ahead (setAhead()): bytes of the file from aheadAt
     * on, read a sector-aligned run at a time. */
    uint8_t*    ahead = nullptr;
    uint32_t    aheadCap = 0, aheadAt = 0, aheadLen = 0;

    /// readWhole() through @p buf (@p cap bytes, a multiple of 512; the
    /// caller's, null for none): one card read a run of it instead of one a
    /// chunk. A 4 kB chunk at the WAV header's 44-byte offset was three card
    /// commands -- a partial sector, the whole ones, a partial one --, and
    /// eight tracks of them filled the card's time beside the sampler's
    /// streamer (hil_track_transitions.py, 2026-10-27).
    void setAhead(uint8_t* buf, uint32_t cap) {
        ahead = buf;
        aheadCap = buf ? cap : 0;
        aheadLen = 0;
    }

    void close() {
        if (fd >= 0) fsx::close(fd);
        fd = -1;
        path.clear();
        cursor = UINT32_MAX;
        heldAt = UINT32_MAX;
        aheadLen = 0;
    }

    bool readAll(void* dst, size_t n) { return fsx::readSome(fd, dst, n) == (long)n; }

    bool open(const std::string& p) {
        close();
        fd = fsx::openRead(p);
        if (fd < 0) return false;
        uint8_t h[12];
        if (!readAll(h, 12) || std::memcmp(h, "RIFF", 4) || std::memcmp(h + 8, "WAVE", 4)) {
            close();
            return false;
        }
        bool fmtOk = false;
        uint32_t at = 12;
        for (;;) {
            uint8_t ch[8];
            if (!readAll(ch, 8)) break;
            at += 8;
            const uint32_t size = ch[4] | ch[5] << 8 | ch[6] << 16 | (uint32_t)ch[7] << 24;
            if (!std::memcmp(ch, "fmt ", 4)) {
                uint8_t fm[16];
                if (size < 16 || !readAll(fm, 16)) break;
                channels = fm[2] | fm[3] << 8;
                const uint16_t bits = fm[14] | fm[15] << 8;
                fmtOk = (fm[0] | fm[1] << 8) == 1 && bits == 16 && (channels == 1 || channels == 2);
                at += size + (size & 1);
                if (size > 16 && !fsx::seek(fd, at)) break;
            } else if (!std::memcmp(ch, "data", 4)) {
                if (!fmtOk) break;
                dataOffset = at;
                frames = size / (channels * 2u);
                path = p;
                cursor = 0;
                return true;
            } else {
                at += size + (size & 1);
                if (!fsx::seek(fd, at)) break;
            }
        }
        close();
        return false;
    }

    /// Up to @p n stereo frames from @p frame on; the rest past the file's end
    /// is silence. Returns frames written (always n).
    uint32_t read(uint32_t frame, int16_t* dst, uint32_t n) {
        readWhole(frame, dst, n);
        return n;
    }

    /// read(); false when the card gave fewer of those frames than the file
    /// says it holds -- cut short, or a read that failed.
    bool readWhole(uint32_t frame, int16_t* dst, uint32_t n) {
        uint32_t got = 0, want = 0;
        if (fd >= 0 && frame < frames) {
            want = std::min(n, frames - frame);
            const uint32_t fb = channels * 2u;
            if (ahead) {
                got = readAhead(frame, dst, want, fb);
            } else {
                if (cursor != frame) {
                    fsx::seek(fd, dataOffset + frame * channels * 2u);
                    cursor = frame;
                }
                const long r = fsx::readSome(fd, dst, want * fb);
                got = r > 0 ? (uint32_t)r / fb : 0;
                cursor = r > 0 && (uint32_t)r == got * fb ? cursor + got : UINT32_MAX;
            }
            if (channels == 1) {
                for (int32_t i = (int32_t)got - 1; i >= 0; --i) dst[i * 2] = dst[i * 2 + 1] = dst[i];
            }
        }
        if (got < n) std::memset(dst + got * 2, 0, (n - got) * 4);
        return got == want;
    }

    /// readWhole() of a file played at another speed: @p n stereo frames from
    /// @p pos on, @p step apart -- positions in the file with kVarispeedFracBits
    /// fraction bits (varispeed_step()) -- each the two frames around it
    /// interpolated in a straight line: a stop-gap until a bake at the song's
    /// tempo replaces it, so plain and cheap. Read @p scratchFrames frames
    /// (at least kVarispeedGuard) at a time through @p scratch; past the
    /// file's end is silence.
    bool readVarispeed(uint64_t pos, uint64_t step, int16_t* dst, uint32_t n, int16_t* scratch, uint32_t scratchFrames) {
        constexpr int kFracBits = 15;   // of the weight: (difference x weight) stays inside 32 bits
        bool whole = true;
        for (uint32_t k = 0; k < n;) {
            const uint64_t x = pos + (uint64_t)k * step;
            const uint32_t first = (uint32_t)(x >> kVarispeedFracBits);
            /* As many frames played as have both their file frames in scratch. */
            const uint64_t limit = ((uint64_t)first + scratchFrames - 1) << kVarispeedFracBits;
            const uint32_t m = (uint32_t)std::max<uint64_t>(1, std::min<uint64_t>(n - k, (limit - x + step - 1) / step));
            const uint32_t last = (uint32_t)((x + (uint64_t)(m - 1) * step) >> kVarispeedFracBits) + 1;
            whole = source(first, last - first + 1, scratch) && whole;
            for (uint32_t j = 0; j < m; ++j) {
                const uint64_t xj = x + (uint64_t)j * step;
                const int16_t* a = scratch + ((uint32_t)(xj >> kVarispeedFracBits) - first) * 2;
                const int32_t w = (int32_t)((xj & ((1ull << kVarispeedFracBits) - 1)) >> (kVarispeedFracBits - kFracBits));
                int16_t* d = dst + (size_t)(k + j) * 2;
                d[0] = (int16_t)(a[0] + (((a[2] - a[0]) * w) >> kFracBits));
                d[1] = (int16_t)(a[1] + (((a[3] - a[1]) * w) >> kFracBits));
            }
            k += m;
        }
        return whole;
    }

private:
    /// @p want frames of @p fb bytes from @p frame on into @p dst, as they are
    /// in the file, through the read-ahead; the frames it got.
    uint32_t readAhead(uint32_t frame, int16_t* dst, uint32_t want, uint32_t fb) {
        uint32_t done = 0;
        while (done < want) {
            const uint32_t at = dataOffset + (frame + done) * fb;
            if (at < aheadAt || at + fb > aheadAt + aheadLen) {
                const uint32_t from = at & ~511u;
                cursor = UINT32_MAX;
                aheadLen = 0;
                if (!fsx::seek(fd, from)) break;
                const long r = fsx::readSome(fd, ahead, aheadCap);
                if (r <= 0) break;
                aheadAt = from;
                aheadLen = (uint32_t)r;
                if (at + fb > aheadAt + aheadLen) break;   // the file ends inside this frame
            }
            const uint32_t k = std::min(want - done, (aheadAt + aheadLen - at) / fb);
            std::memcpy(dst + (size_t)done * channels, ahead + (at - aheadAt), (size_t)k * fb);
            done += k;
        }
        return done;
    }

    /// Frames [@p first, @p first + @p count) of the file into @p s, the two held
    /// ones taken where they fall in it; at least kVarispeedGuard of them,
    /// whose last two are held after.
    bool source(uint32_t first, uint32_t count, int16_t* s) {
        uint32_t have = 0;
        if (heldAt != UINT32_MAX && first >= heldAt && first < heldAt + kVarispeedGuard) {
            have = std::min(heldAt + kVarispeedGuard - first, count);
            std::memcpy(s, held + (first - heldAt) * 2, have * 4);
        }
        const bool whole = have == count || readWhole(first + have, s + have * 2, count - have);
        heldAt = first + count - kVarispeedGuard;
        std::memcpy(held, s + (count - kVarispeedGuard) * 2, sizeof held);
        return whole;
    }
};

/// What playback plays: the song flattened to sample ranges, rebuilt on
/// every commit. The worker reads it under snapMutex_; RT never does.
/// A block as samples of the song, and where in its file it starts.
/// @c step: an element's bake made at another tempo, read varispeed
/// (varispeed_step(); @c offset is then in frames of the song); 0 reads it as it is.
struct Segment { std::string path; uint32_t start = 0, end = 0, offset = 0; uint64_t step = 0; };
struct Arrangement {
    std::vector<Segment> tracks[kTracks];
    /// The context stem under an element being edited: one segment, played at
    /// unity -- no fader, no meter (requestContext()).
    std::vector<Segment> context;
    /// The seq track's blocks, for the sequencer to play under (empty with
    /// seqMapped false: a seq track with no blocks loops over everything).
    std::vector<PatternSequencer::SongSpan> seqSpans;
    bool     seqMapped = false;
    uint32_t end = 0;             // samples; 0 = empty song
    uint32_t samplesPerBar = 0;
    bool     loop = true;
    uint32_t loopA = 0, loopB = 0;   // the loop region, samples; loopB <= loopA = none
};

/// Where a play position goes once it reaches the wrap: into the loop
/// region when there is one, else back to the song's start. One rule, used
/// by RT for what is heard and by the worker for what it reads next, so the
/// two never disagree about where the song is.
inline uint32_t wrap_position(uint32_t pos, uint32_t end, uint32_t loopA, uint32_t loopB)
{
    if (loopB > loopA) return pos >= loopB ? loopA + (pos - loopB) % (loopB - loopA) : pos;
    return end ? pos % end : 0;
}

/// Marks the render inside this node: the counter is odd from entry to exit,
/// so the worker can wait for a block in flight before freeing what RT reads.
struct RtSection {
    std::atomic<uint32_t>& gen;
    explicit RtSection(std::atomic<uint32_t>& g) : gen(g) { gen.fetch_add(1, std::memory_order_seq_cst); }
    ~RtSection() { gen.fetch_add(1, std::memory_order_seq_cst); }
};

/* Armed: primed and waiting for the sequencer's clock, which the song starts
 * with when it plays in sync (see startPlayback()). Counting: primed, and
 * playing its own count-in before it starts (a song that is not in sync). */
enum class Play : uint8_t { Idle, Priming, Armed, Counting, Running, Ended };

/// Readers a disk bake holds at once, beside its writer: the card's descriptors
/// are shared with playback and the kit's samples, and a bake keeps to three.
constexpr int kBakeReaders = 2;
/// A disk bake sums this many chunks of its sources, one source at a time,
/// before it writes them: four tracks heard at once then open two files a
/// span, where a chunk at a time opened two a chunk.
constexpr uint32_t kMixSpanChunks = 4;

/// The files a disk bake reads, in kBakeReaders descriptors: one asked for that
/// none holds is opened in place of the one read longest ago.
struct BakeReaders {
    ClipReader slot[kBakeReaders];
    uint32_t   used[kBakeReaders] = {};
    uint32_t   tick = 0;

    bool holds(const std::string& path) const {
        for (const ClipReader& r : slot)
            if (r.fd >= 0 && r.path == path) return true;
        return false;
    }
    ClipReader* get(const std::string& path) {
        int pick = 0;
        for (int i = 0; i < kBakeReaders; ++i) {
            if (slot[i].fd >= 0 && slot[i].path == path) { pick = i; break; }
            if (used[i] < used[pick]) pick = i;
        }
        used[pick] = ++tick;
        if (slot[pick].fd >= 0 && slot[pick].path == path) return &slot[pick];
        return slot[pick].open(path) ? &slot[pick] : nullptr;
    }
};

/// A disk bake under way (worker only): an element's bake or a context stem,
/// mixed a piece at a time so the song can stream between pieces -- a read of
/// one source over at most a chunk, or one chunk written. Its writer and its
/// readers stay open between pieces: three descriptors.
struct MixJob {
    enum class For : uint8_t { Element, Context };
    For what = For::Element;
    /* An element's: which, and its bake as the queue names it. */
    std::string ref;
    ResolvedElement bake;
    /* A stem's: the request it is for and what contextDone() is told. */
    uint32_t gen = 0, at = 0, frames = 0;
    float bpm = 0.0f;
    ContextState state = ContextState::None;
    std::string why, focus;
    /* The mix: its pieces by start, the frames it writes, where to, and the
     * source that could not be read whole, when one could not. */
    std::vector<BakedPiece> pieces;
    uint32_t total = 0;
    std::string path, lost;
    bool noMemory = false;          ///< no room for the span's sum: it never began
    WavWriter w;
    BakeReaders readers;
    size_t first = 0;               ///< pieces before it end before the span
    uint32_t spanAt = 0, spanLen = 0;   ///< the span being summed; 0 long: none yet
    std::vector<size_t> live;       ///< the pieces under it, in the order they are read
    size_t next = 0;                ///< the one being read; live.size(): the span is being written
    uint32_t readAt = 0;            ///< the next frame of it to read
    uint32_t written = 0;           ///< frames of the span written
};

} // namespace songdetail

using namespace songdetail;

struct SongEngine::Impl {
    Impl(ISongEnginePlatform& platform, MetronomeNode& clock, PatternSequencer& sequencer);
    ISongEnginePlatform& p;
    MetronomeNode& clk_;
    PatternSequencer& seq_;
    const SongEngineSizes sz;
    static SongEngineSizes checkedSizes(SongEngineSizes s);
    /// A bounce's first piece fills up the chunk the WAV header starts, so
    /// every piece after it ends on a chunk boundary of the file.
    uint32_t firstChunkFrames() const { return (sz.chunkFrames * 4 - WavWriter::kHeaderBytes) / 4; }
    void logf(bool warning, const char* fmt, ...);
    void shutdown();
    void onPrepare(uint32_t maxFrames, uint32_t sampleRate);
    void onSampleRateChange(uint32_t sr) { sampleRate_ = sr ? sr : 44100; }
    void mix(float* out, uint32_t frames);
    uint32_t countIn(float* out, uint32_t frames);
    /// The pads bus, one block. Only while a bounce is armed.
    bool capturing() const { return bArmed_.load(std::memory_order_seq_cst); }
    bool captureBacklogged() const { return bRing_.cap && bRing_.used() * 2 > bRing_.cap; }
    void rtCapture(const float* bus, uint32_t frames);
    bool ensureWorker();
    // ── Projects ──────────────────────────────────────────────────────────
    bool requestProjects() { return request(kListProjects); }
    std::vector<std::string> projects();
    bool requestOpen(const std::string& name);
    bool requestOpenElement(const std::string& ref);
    bool requestElements() { return request(kListElements); }
    std::vector<ElementInfo> elements();
    bool requestBakeElement();
    ElementBake bakeResult();
    bool requestElementFromRegion(uint32_t fromBar, uint32_t toBar);
    bool requestElementFromSong(const std::string& name, const std::vector<std::string>& openElements);
    bool requestElementDiscard(const std::string& ref);
    bool requestUses();
    std::vector<ElementUse> usesOf(const std::string& ref);
    bool requestSnapshot(const std::string& ref) { return requestSnapshotEdit(ref, kEditSnapshot); }
    bool requestEditHash();
    uint64_t editHash() { return editHash_.load(); }
    bool requestRestore(const std::string& ref) { return play_.load() == Play::Idle && requestSnapshotEdit(ref, kEditRestore); }
    bool requestDropSnapshot(const std::string& ref) { return requestSnapshotEdit(ref, kEditDropSnapshot); }
    bool snapshotPending(const std::string& ref);
    bool usesPending() const { return (int32_t)(usesAsked_.load() - usesAnswered_.load()) > 0; }
    ElementUses usesIndex();
    bool scenesSaved() const { return !seq_.dirty(); }
    bool requestCopyElements(const std::vector<ElementFork>& forks, const std::vector<std::string>& rewrites,
                             const std::map<std::string, std::string>& copied);
    std::map<std::string, std::string> elementsCopied();
    bool requestDeleteElement(const std::string& ref);
    std::string elementMade();
    ElementClipState elementClipState(uint8_t track, size_t clip);
    std::string elementClipWhy(uint8_t track, size_t clip);
    bool elementClipRebakes(uint8_t track, size_t clip);
    uint8_t backgroundProgress() const { return bakeProgress_.load(); }
    bool backgroundBusy() const { return relinkReq_.load() || relinking_.load() || bakeQueued_.load() != 0; }
    void idleBaking(bool allowed);
    bool requestContext(const ContextRequest& r);
    void contextSolo(bool on) { ctxSolo_.store(on); }
    /// A stem made at another tempo than the song plays now is not heard: Incomplete, until the tempo is back.
    ContextState contextState() const {
        const ContextState s = ctxState_.load();
        return ctxApart_.load() && s == ContextState::Ready ? ContextState::Incomplete : s;
    }
    std::string contextWhy();
    SongOpState openState() const { return openState_.load(); }
    bool requestRename(const std::string& name);
    bool requestDelete();
    bool requestTempo(float bpm);
    SongOpState editState() const { return editState_.load(); }
    bool requestMerge(uint32_t trackMask, uint32_t fromBar, uint32_t toBar);
    uint8_t editProgress() const { return editProgress_.load(); }
    /// Counted when asked, not when the worker last had time to: right after an
    /// open or a tempo says Ready it is what that song waits for.
    uint32_t stretchWaiting() { return nextStretch(nullptr); }
    uint8_t stretchProgress() const { return stretchProg_.load(); }
    bool undo() { return step(undo_, redo_); }
    bool redo() { return step(redo_, undo_); }
    uint32_t undoSteps() const { return undoCount_.load(); }
    uint32_t redoSteps() const { return redoCount_.load(); }
    void trackLevels(int t, float& left, float& right);
    bool requestExportLiveSet();
    std::vector<std::string> editReport();
    bool clipPeaks(const std::string& file, ClipPeaks& out);
    uint32_t peaksGeneration() const { return peaksGen_.load(); }
    const char* debugLine();
    char dbg_[512] = {};
    /* ASONG_DBG's margins: see debugLine(). */
    std::atomic<uint32_t> dbgRingMin_{UINT32_MAX};
    std::atomic<uint32_t> dbgGapMaxUs_{0};
    std::atomic<uint32_t> dbgReadMaxUs_{0};
    std::atomic<uint32_t> dbgSaveMaxUs_{0};
    std::atomic<uint32_t> dbgYieldMaxUs_{0};       // a caller's wait in yieldFreeze()
    std::atomic<int64_t>  yieldHoldUs_{0};         // no freeze starts before then (kYieldHoldUs)
    std::atomic<uint32_t> dbgFzSetupMaxUs_{0};     // runBounce() before its first piece of card
    std::atomic<uint32_t> dbgFzTailMaxUs_{0};      // runBounce() from the take's end to its return
    std::atomic<uint32_t> dbgFzLockMaxUs_{0};      // runFreeze()'s last pass under songMutex_
    std::atomic<uint32_t> dbgFillMaxUs_{0};        // one fillRings()
    std::atomic<uint32_t> dbgBgMaxUs_{0};          // one backgroundStep() while the song streams
    std::atomic<uint32_t> dbgOpenMaxUs_{0};        // a playing track's clip opened
    /* What the worker is doing (WorkerAt), and the song's underruns by what
     * it was doing when RT found the ring empty: ASONG_DBG's under_at=. */
    enum WorkerAt : uint8_t { AtWait, AtFill, AtRead, AtOpen, AtYield, AtBackground, AtTake, AtOther, kWorkerAts };
    std::atomic<uint8_t>  workerAt_{AtOther};
    std::atomic<uint32_t> underAt_[kWorkerAts] = {};
    int64_t lastFillUs_ = 0;                       // worker: the last top-up, 0 while not streaming
    static void dbgMax(std::atomic<uint32_t>& a, int64_t v);
    uint32_t framesPerBar();
    int audioTarget(uint8_t track, bool make);
    bool requestImport(const std::string& file, uint32_t start, uint32_t end,
                       uint8_t track, uint32_t bar, uint32_t lead);
    AudioSong& song();
    std::recursive_mutex& songMutex() { return songMutex_; }
    void commit();
    bool requestClips() { return request(kListClips); }
    std::vector<ClipInfo> clips();
    uint32_t listGeneration() const { return listGen_.load(); }
    uint32_t openGeneration() const { return openGen_.load(); }
    uint32_t playEndBars();
    bool bounceSeqTrack(uint8_t track);
    bool freezeTrack(uint8_t track);
    uint8_t renderScene(const SongTrack& t) const;
    FreezeState freezeState(uint8_t track);
    bool bounceScene(uint8_t scene, uint8_t track) { return requestBounce(scene, track, 0); }
    bool requestBounce(uint8_t scene, uint8_t track, uint32_t bars, uint32_t fromBar = 0,
                       uint32_t freezeKey = 0, bool toAudio = false);
    BounceStatus bounceStatus();
    void cancelBounce() { bCancel_.store(true); }
    bool playCountedIn(uint32_t fromBar, SyncTake take, uint8_t beats);
    uint8_t countInBeatsLeft() const;
    bool yieldFreeze();
    bool playWithTake(uint32_t fromBar, SyncTake take);
    void stop();
    bool playing() const;
    double positionBars() const;
    uint32_t framesPlayed() const { return played_.load(std::memory_order_acquire); }
    uint32_t positionFrames() const { return pos_.load(std::memory_order_relaxed); }
    uint32_t streamAheadFrames() const { return streamingNow() ? ring_.used() : UINT32_MAX; }
    uint32_t underruns() const { return underruns_.load(); }
    uint32_t openFailures() const { return openFailures_.load(); }
    bool recordSeq(uint32_t fromBar, SyncTake take, uint8_t beats);
    bool seqPunch(SyncTake take);
    SyncTake seqTake() const { return seqTake_.load(); }
    bool openSeqEdit(uint8_t track, uint32_t bar, uint8_t scene);
    void syncSeqEdit() { syncSeqEditNow(false); }
    void syncSeqEditNow(bool evenRecording);
    void closeSeqEdit();
    int32_t seqEditBar() const { return editTrack_ >= 0 ? (int32_t)editBar_ : -1; }
    bool seqPunchable() const;
    void releaseRings();
    bool liveStart();
    bool liveLaunch(uint8_t track, const std::string& file, uint16_t bars);
    void liveQuant(uint8_t bars) { liveQuant_.store(bars ? bars : 1); }
    bool liveStop(uint8_t track);
    bool liveState(ISongEngine::LiveState& out);
private:
    /* Only playback is a standing job; a bounce is a request of its own, so a
     * stop or a play arriving beside it can never overwrite it. */
    enum class Job : uint8_t { None, Play };
    static constexpr uint8_t kListProjects = 1;
    static constexpr uint8_t kListClips    = 2;
    static constexpr uint8_t kListElements = 4;
    static constexpr uint8_t kListUses     = 8;
    /// Lists that wait for the stop while the song streams: the elements'
    /// and the uses' read every song.json, longer than the ring lasts.
    static constexpr uint8_t kListsAfterStop = kListElements | kListUses;
    static void worker_entry(void* arg) { static_cast<Impl*>(arg)->worker(); }
    bool request(uint8_t list);
    bool claimEdit();
    bool requestEdit(uint8_t edit);
    void waitRtOut();
    void doListProjects();
    void doListClips();
    void doListElements();
    void doListUses();
    /// @p remember: @p name is the project to open at the next boot (an
    /// element is not).
    SongOpState doOpen(const std::string& name, bool remember = true);
    bool askToOpen(const std::string& name, bool remember);
    SongOpState doBakeElement();
    SongOpState doElementFromRegion();
    SongOpState doElementFromSong();
    SongOpState doElementDiscard();
    SongOpState doSnapshot();
    SongOpState doRestore();
    SongOpState doDropSnapshot();
    SongOpState doEditHash();
    SongOpState doCopyElements();
    SongOpState doDeleteElement();
    /// The snapshot of the element at @p dir back over its files, then taken
    /// off -- see ISongEngine::requestRestore. False with it still there to
    /// restore from again.
    bool restoreSnapshot(const std::string& dir);
    /// The snapshot files in the element folder @p dir taken off; none is no failure.
    bool dropSnapshotFiles(const std::string& dir);
    /// @p dir's snapshot is there and is byte for byte the files it was kept of.
    bool snapshotUnchanged(const std::string& dir);
    bool requestSnapshotEdit(const std::string& ref, uint8_t edit);
    /// A ref no element on the card has, with its folder made under the
    /// library (its clips/ too when @p withClips). Empty when the card would
    /// not; @p dir is then whatever was made, to be removed.
    std::string makeElementFolder(std::string& dir, bool withClips);
    /// Worker: an element that was not made -- @p why in the report, the
    /// folder @p dir (when one was started) taken off the card again.
    SongOpState elementFailed(const std::string& why, const std::string& dir);
    /// How element hashes and the resolver look at the card: sizes and times
    /// of files (a folder is no file), kit.json read whole, kit folders listed.
    static ElementHashReader cardHashReader();
    // ── Element blocks against their elements (SongEngineDiskBake.cpp) ────
    /// What a relink pass of @p s depends on: its folder and tempo, and every
    /// element block's element, place, bake and pass length.
    static std::string elementSignature(const AudioSong& s);
    /// The open song's element blocks linked to their elements' bakes as they
    /// are now; the ones that can be mixed from the card queued.
    void doRelink();
    /// A pass that gave way: nothing it found is kept, and it runs again.
    void relinkGaveWay();
    /// The queue's first bake started, made or given up -- or left where it is
    /// when a request comes first.
    void doDiskBake();
    /// Another song's bakes wait for the player to be away.
    bool diskBakePending() { return !bakeQueue_.empty() && p.storageReady() && (!bakeOther_ || idleNow()); }
    /// How a disk bake's mix ended: Done, given way to a request (made again
    /// after it), or Failed with no file left behind.
    enum class BakeRun : uint8_t { Done, GaveWay, Failed };
    /// A mix of @p pieces into @p total frames at @p path, for what @p job
    /// says, begun: the next pieces are stepMix()'s. One that cannot start
    /// has ended Failed.
    void startMix(std::unique_ptr<MixJob> job, std::vector<BakedPiece> pieces, uint32_t total, const std::string& path);
    /// One piece of the mix under way.
    void stepMix();
    /// The mix under way ended @p run: on Done its file in place, else its
    /// .part removed; then what it was for told.
    void endMix(BakeRun run);
    bool mixing(MixJob::For what) const { return mix_ && mix_->what == what; }
    /// A request the loop takes next would change what the mix reads or
    /// writes, or another song's bake may not go on: it ends first (and is
    /// made again later). A stop, a play, a save or a list only pause it.
    bool mixMustEnd();
    /// The resolver's answer for @p ref, or the bake this queue has made of it since.
    const ResolvedElement& bakeView(const std::string& ref);
    void bakeMade(const std::string& ref, const ResolvedElement& e);
    void bakeFailed(const std::string& ref, const std::string& why);
    /// Blocks of each element in @p links, in the song @p dir the queue is
    /// for, take that bake, pass length and @p bpm, the tempo the bakes were
    /// resolved at (SongClip::bakeBpm): in the open song committed and saved,
    /// but no undo step; in another song on the card (linkOnCard()).
    void linkBlocks(const std::string& dir, const std::map<std::string, std::pair<std::string, uint32_t>>& links,
                    float bpm);
    /// The song.json in @p dir read, its blocks of each element in @p links
    /// given that bake and @p bpm, and written back when one changed.
    void linkOnCard(const std::string& dir, const std::map<std::string, std::pair<std::string, uint32_t>>& links,
                    float bpm);
    void dropBakeQueue();
    // ── The bake cache's size (SongEngineDiskBake.cpp) ─────────────────────
    /// Worker: the cache inside its cap -- see the file comment. A pass that gives
    /// way is asked for again; one that could not tell what the open song plays
    /// removes nothing.
    void doEvict();
    /// What the open song's tree plays, every element's newest bake, and the
    /// stem in use: the files no pass may remove. False when the work gave way
    /// before it knew them all.
    bool protectedBakes(std::set<std::string>& keep);
    /// @p file just used: the newest in the last-use counters. Written by saveUsage().
    void bumpUse(const std::string& file);
    void loadUsage();
    void saveUsage();
    /// @p ref's own song.json names @p file as its newest bake -- once the song
    /// stops, when it streams (newestDue_).
    void noteNewestBake(const std::string& ref, const std::string& file);
    /// The same now: on the card, or in memory when @p ref is the song open.
    void writeNewestBake(const std::string& ref, const std::string& file);
    /// One newest bake noted while the song streamed, written.
    void writeNewestDue();
    /// The open song, an element, has made @p file from itself.
    void ownBake(const std::string& file);
    // ── Background work and playback (SongEngineDiskBake.cpp) ─────────────
    bool streamingNow() const { return job_.load() == Job::Play && play_.load() != Play::Idle; }
    /// Worker, between two card accesses of background work: while the song
    /// streams, its ring is topped up whenever it is below half, so the work
    /// reads only with half a ring in hand; then the streamer's turn. Sets
    /// gaveWay_ when a request the loop takes is waiting, the song came to
    /// its end, or idle work is no longer allowed: the work stops, and what
    /// it found is not used.
    void pace();
    /// cardHashReader() with pace() before every access; once gaveWay_ is set
    /// each access fails, so a resolve under way ends at once.
    ElementHashReader pacedHashReader();
    /// fsx::isFile() the same way.
    bool pacedIsFile(const std::string& path, uint64_t atLeast = 0);
    /// Anything for backgroundStep() while nothing plays.
    bool backgroundPending();
    /// One piece of the background's work, in the queue's order: the context
    /// stem, the relink pass, a mix under way, (stopped) waveforms and
    /// stretched copies, the open song's element bakes, (idle) other songs'.
    void backgroundStep(bool streaming);
    // ── Idle baking (SongEngineDiskBake.cpp) ──────────────────────────────
    bool idleNow() const { return idleAllowed_.load() && !streamingNow(); }
    bool idlePending() { return idleNow() && !changed_.empty() && p.storageReady(); }
    /// Elements whose other songs may play an older bake now.
    void noteChanged(const std::set<std::string>& refs);
    /// The next step of idle baking: the uses read, the songs that play a
    /// changed element found, each relinked in turn.
    void idleStep();
    /// The songs (not the open one) that play an element in @p refs, through
    /// the elements that hold it, as the last uses scan has them.
    std::vector<std::string> songsPlaying(const std::set<std::string>& refs);
    /// The song @p name relinked on the card, as a pass relinks the open one.
    /// False when it gave way, to be done again.
    bool relinkOther(const std::string& name);
    /// A song opened: nothing the last one's pass found holds.
    void forgetElements();
    // ── The context stem (SongEngineDiskBake.cpp) ─────────────────────────
    bool contextPending() { return ctxPending_.load() && p.storageReady(); }
    /// The stem asked for last, found in the cache or mixed from the card --
    /// or left where it is when a request comes first.
    void doContext();
    /// What a stem bake for request @p gen came to: @p path (empty: nothing
    /// to play) over [@p at, @p at + @p frames) of the open song, mixed at
    /// @p bpm. Dropped when another request or a drop came since.
    void contextDone(uint32_t gen, const std::string& path, uint32_t at, uint32_t frames, float bpm,
                     ContextState state, const std::string& why);
    /// No stem to play: @p state and @p why published, a bake under way for
    /// an older request left to be dropped. Caller holds songMutex_.
    void dropContext(ContextState state, const std::string& why);
    /// The stem's segment as playback lays it: none while there is no stem,
    /// in another song, or with the song at another tempo than the stem was
    /// mixed at. Caller holds songMutex_.
    std::vector<Segment> contextSegments() const;
    /// The open song plays at another tempo than its stem was mixed at (a tempo
    /// turned inside the element), so the stem is not heard. Caller holds songMutex_.
    bool contextApart() const;
    /// The stem handed to playback now. Caller holds songMutex_.
    void publishContext();
    void openLast();
    SongOpState doRename();
    SongOpState doDelete();
    SongOpState doTempo(float bpm);
    SongOpState doMerge(uint32_t mask, uint32_t fromBar, uint32_t toBar);
    SongOpState doExport();
    SongOpState doImport();
    void doSave();
    /// The sequencer's scenes, as SeqFreezeKey.hpp asks for them.
    SceneLookup scenes() const;
    /// song_end_bars(), seq_freeze_range() and seq_freeze_key() over the sequencer's scenes.
    uint32_t endBars(const AudioSong& song) const;
    void seqRange(const AudioSong& song, int track, uint32_t& from, uint32_t& to) const;
    uint32_t seqKey(const AudioSong& song, int track) const;
    /// One render of a freeze: the song's bars [w0, w0 + pre + cap) of the
    /// track, the first @p pre not recorded (what rings into the bars that
    /// are), and the audio blocks it gives -- offsets into its file.
    struct FreezePiece {
        uint32_t w0 = 0, pre = 0, cap = 0;
        std::vector<SongClip> emit;
    };
    static constexpr uint32_t kFreezePreBars  = 1;   ///< rung into the first bar recorded
    static constexpr uint32_t kFreezeTailBars = 1;   ///< what rings on after a block, into a gap
    std::vector<FreezePiece> freezePlan(const AudioSong& song, int track, uint32_t spb) const;
    std::vector<PatternSequencer::SongSpan> windowSpans(const AudioSong& song, int track, uint32_t spb,
                                                        uint32_t w0, uint32_t w1) const;
    uint32_t pieceKey(const AudioSong& song, int track, uint32_t spb, const FreezePiece& pc,
                      const std::vector<PatternSequencer::SongSpan>& spans) const;
    void runFreeze();
    bool frozenPlays(const AudioSong& song, int track) const;
    /// Worker: the seq tracks of @p s frozen through a kit or an instrument the
    /// card holds otherwise now (SongTrack::freezeKit) -- saved under its name
    /// since, maybe in another boot, where no save was heard.
    static std::vector<int> kitChangedFreezes(const AudioSong& s);
    /// Each of @p tracks of @p s plays its freeze no longer, and its next one
    /// renders anew rather than finding the old renders by their names (as
    /// SongSounds leaves a track whose kit it hears saved).
    static void unfreeze(AudioSong& s, const std::vector<int>& tracks);
    Arrangement flatten(const AudioSong& song) const;
    std::vector<PatternSequencer::SongSpan> seqSpansOf(const SongTrack& tr, uint32_t spb, uint32_t tpb,
                                                       uint32_t cut, uint8_t route = 0) const;
    PatternSequencer::SongNotes songNotes(const SeqPatternPtr& p, uint32_t spb, uint32_t tpb) const;
    void pushSeqMap();
    // ── Undo ──────────────────────────────────────────────────────────────
    /// An arrangement as bytes, and the seq blocks' notes it names by index.
    /// Notes are shared and never changed in place, so a step holding the
    /// pointer holds them as they were, and costs nothing until they are
    /// replaced. Equal when the bytes and the very same notes are.
    struct Snapshot {
        std::vector<uint8_t> bytes;
        std::vector<SeqPatternPtr> notes;
        bool empty() const { return bytes.empty(); }
        bool operator==(const Snapshot& o) const { return bytes == o.bytes && notes == o.notes; }
    };
    static void pack(const AudioSong& s, Snapshot& out);
    static void unpack(const Snapshot& in, AudioSong& s);
    void noteChange();
    bool songJobRunning();
    bool step(std::vector<Snapshot>& from, std::vector<Snapshot>& to);
    void meterPut(int t, uint32_t hop, const int32_t m[2]);
    void meterReset();
    void rebuild();
    void applySeqSound();
    bool openTake(SyncTake take, uint32_t fromBar);
    static int blockAt(const SongTrack& tr, uint32_t bar);
    static uint32_t roomAt(const SongTrack& tr, uint32_t bar);
    static bool samePattern(const SeqPatternPtr& a, const SeqPatternPtr& b);
    void closeTake();
    SeqPatternPtr scenePattern(uint8_t scene) const;
    void serviceTake();
    void landLaps(uint16_t upto);
    bool landTake(const std::vector<PatternSequencer::SongTakeNote>& notes, bool replace,
                  uint32_t from, uint32_t to);
    uint32_t takeFillEnd(uint32_t fromBar, uint32_t last) const;
    void finishTake();
    void setStatus(BounceState st, const std::string& err = std::string());
    void worker();
    /// Where the bake named @p file lives: one cache for every song, under the songs root.
    std::string bakePath(const std::string& file) const;
    /// @p n frames of @p r from @p clipFrame of a segment or piece read at
    /// @p step (Segment::step): readWhole() at 0, else varispeed through vsrc_.
    /// Worker only.
    bool readAt(ClipReader& r, uint32_t clipFrame, uint64_t step, int16_t* dst, uint32_t n);
    bool stretchReady(const std::string& path) const;
    struct StretchJob { std::string src, dst; uint32_t offset = 0, frames = 0, outFrames = 0; bool repitch = false; };
    uint32_t nextStretch(StretchJob* job);
    bool stretchPending() { return nextStretch(nullptr) != 0; }
    void doStretch();
    /// A request the loop takes before any background work -- while the song
    /// @p streaming, not what waits for the stop.
    bool requestTaken(bool streaming) const;
    /// requestTaken(), and while nothing plays a relink pass or a context stem
    /// asked for: what a waveform or a stretched copy gives way to.
    bool requestWaiting(bool streaming = false) const;
    bool peaksPending();
    void doPeaks();
    bool readPeaks(const std::string& wav, std::vector<uint8_t>& out);
    /* A .pk file: "CPPK", version, the hop, the WAV's size in bytes when it
     * was made, the count, the values. Little-endian. */
    static constexpr uint8_t kPeaksVersion = 1;
    static constexpr size_t  kPeaksHeader  = 16;
    bool loadPeaks(const std::string& path, uint64_t wavBytes, std::vector<uint8_t>& out);
    void savePeaks(const std::string& dir, const std::string& path, uint64_t wavBytes,
                   const std::vector<uint8_t>& v);
    void startLive();
    void liveApply(int t, uint32_t at);
    void startPlayback(uint32_t fromBar);
    void endPlayback();
    void fillRings();
    void runBounce();
    // Song model and its flattened copy (worker).
    AudioSong   song_;
    std::recursive_mutex songMutex_;
    std::atomic<bool> opened_{false};
    bool        lastTried_ = false;            // worker only
    std::string openName_;
    bool        openRemember_ = true;             // under songMutex_, with openName_
    std::atomic<bool> openReq_{false}, saveReq_{false}, playReq_{false};
    std::atomic<int64_t> saveAtUs_{0};            // a committed song is saved from then on
    std::atomic<SongOpState> openState_{SongOpState::Idle};
    std::atomic<uint32_t> playFrom_{0};
    std::atomic<SyncTake> playTake_{SyncTake::None};
    SyncTake    take_ = SyncTake::None;          // worker: what the sequencer starts doing
    /* A take onto the seq track (recordSeq, seqPunch): what it is, and --
     * under songMutex_ -- its track, the song sample it began on, whether it
     * goes round the region, the laps landed, whether a REC's pass has
     * replaced what was there, the notes moved out of the sequencer and not
     * landed yet, and whether the song runs on past its end for it. */
    std::atomic<SyncTake> seqTake_{SyncTake::None};
    int         takeTrack_ = -1;
    uint32_t    takeFrom_ = 0;
    bool        takeCycle_ = false;
    bool        takeWhole_ = false;              // the cycle is the whole song, not a region
    uint32_t    takeCycleA_ = 0, takeCycleB_ = 0;   // the bars a cycle goes round
    uint16_t    takeLaps_ = 0;
    bool        takeReplaced_ = false;
    std::vector<PatternSequencer::SongTakeNote> takeNotes_;
    bool        takeHorizon_ = false;
    bool        takeBound_ = false;              // worker: the playback running is the take's
    bool        takeRecMute_ = false;            // a REC's pass not landed yet: not heard under it
    /* What applySeqSound() last gave setSeqGain(), kit and pitch; under
     * songMutex_. */
    float       seqGain_[3] = {1.0f, 1.0f, 1.0f};
    float       seqPan_[3] = {0.0f, 0.0f, 0.0f};
    /* The seq view's block (openSeqEdit), under songMutex_: its track (-1
     * none), the bar it starts on, the scene it is edited in, whether it is
     * on the track yet, and the sequencer's edit count last handed over. */
    std::atomic<int> editTrack_{-1};
    uint32_t    editBar_ = 0;
    uint8_t     editScene_ = 0;
    bool        editBound_ = false;
    std::atomic<uint32_t> editGen_{0};
    /* songNotes(): patterns as samples, the oldest dropped first. */
    struct NoteCache { SeqPatternPtr pattern; uint32_t spb, tpb; PatternSequencer::SongNotes notes; };
    mutable std::vector<NoteCache> noteCache_;
    mutable std::mutex noteCacheMutex_;
    static constexpr uint8_t kEditRename = 1, kEditTempo = 2, kEditMerge = 3, kEditImport = 4,
                             kEditDelete = 5, kEditExport = 6, kEditBake = 7, kEditElementRegion = 8, kEditElementSong = 9,
                             kEditElementDiscard = 10, kEditSnapshot = 11, kEditRestore = 12, kEditDropSnapshot = 13,
                             kEditEditHash = 14, kEditCopyElements = 15, kEditDeleteElement = 16;
    uint32_t    importA_ = 0, importB_ = 0, importBar_ = 0, importLead_ = 0;   // owned by claimEdit()'s caller until editReq_
    uint8_t     importTrack_ = 0;
    std::atomic<uint8_t>  editProgress_{0};
    std::atomic<uint32_t> editMask_{0}, editFrom_{0}, editTo_{0};
    std::atomic<uint8_t>  editReq_{0};
    std::atomic<SongOpState> editState_{SongOpState::Idle};
    std::atomic<float>    editBpm_{120.0f};
    std::string editName_;                        // under songMutex_
    std::vector<std::string> editRefs_;           // the breadcrumb an element from a song is checked against, under songMutex_
    std::vector<ElementFork> copyForks_;          // requestCopyElements()'s, under songMutex_
    std::vector<std::string> copyRewrites_;
    std::map<std::string, std::string> copyRefs_;
    std::vector<std::string> report_;             // the last export's, under reportMutex_
    std::mutex  reportMutex_;
    ElementBake bake_;                            // the last bake's, under reportMutex_
    std::string elementMade_;                     // the last element made's ref, under reportMutex_
    std::map<std::string, std::string> copiesMade_;   // the last requestCopyElements()'s, under reportMutex_
    /* The relink pass: asked for by an open and by a rebuild whose element
     * blocks or tempo moved (elementSig_, under songMutex_). What it found of
     * each element the open song's blocks use, and which of those blocks' bakes
     * are on the card, under elementMutex_ -- what elementClipState() reads. */
    struct ElementSeen {
        std::string file;           ///< the element's bake as it is now
        uint32_t    frames = 0;
        bool        exists = false; ///< on the card
        bool        baking = false; ///< queued or being mixed
        std::string why;            ///< what keeps it from being baked from the card
        bool        rebakes = false; ///< and a rebake mends it (ResolvedElement::rebakes)
    };
    std::atomic<bool> relinkReq_{false}, relinking_{false};
    std::string elementSig_;
    std::map<std::string, ElementSeen> elementSeen_;
    std::set<std::string> bakesOnCard_;
    std::mutex elementMutex_;
    /* The disk bakes the last pass queued, the next first (worker only): the
     * resolver it asked, the song and tempo they are for, the ones made and
     * given up since. */
    std::unique_ptr<ElementResolver> resolver_;
    std::vector<std::string> bakeQueue_;
    std::string bakeSong_;
    float       bakeBpm_ = 0.0f;
    std::map<std::string, ResolvedElement> bakesMade_;
    std::map<std::string, std::string> bakesFailed_;
    std::atomic<uint32_t> bakeQueued_{0};
    std::atomic<uint8_t>  bakeProgress_{0};
    bool        bakeOther_ = false;   ///< the queue is another song's (idle baking), not the open one's
    /// The bake cache (worker only): a pass is due, the first has swept the parts an
    /// earlier run left, and the counters of last use (file -> counter, the
    /// largest the last one handed out).
    bool        evictPending_ = false, partsSwept_ = false, usageLoaded_ = false, usageDirty_ = false;
    std::map<std::string, uint64_t> usage_;
    uint64_t    usageClock_ = 0;
    /// Newest bakes noted while the song streamed, by element ref (worker only).
    std::map<std::string, std::string> newestDue_;
    /// The mix under way, and the sum of its span (worker only, held while a
    /// mix is).
    std::unique_ptr<MixJob> mix_;
    int32_t*    mixAcc_ = nullptr;
    bool        gaveWay_ = false;     ///< pace() met a request (worker only)
    bool        idleWork_ = false;    ///< what pace() paces is idle work
    /// The element that was open before the song open now: what was left to
    /// come here, the first of the open song's blocks to be baked.
    std::string leftRef_;
    /* Idle baking (worker only but idleAllowed_): elements changed since the
     * songs playing them were relinked; the songs left to relink for those in
     * idleServing_, found from a uses scan asked for (idleScanAsked_). */
    std::atomic<bool> idleAllowed_{false};
    std::set<std::string> changed_, idleServing_;
    std::vector<std::string> idleSongs_;
    bool        idleListed_ = false;
    bool        idleScanAsked_ = false;
    /* The context stem, under songMutex_: the request and its generation (a
     * newer request or a drop moves it, and a bake for an older one is not
     * played), the song it was asked in, and what plays -- the stem's path,
     * where it starts in the open song, its length and the tempo it was mixed
     * at -- with the state's why. ctxPending_: a bake for it is still to run. */
    ContextRequest ctxReq_;
    uint32_t    ctxGen_ = 0;
    std::string ctxSong_;
    std::string ctxPath_;
    uint32_t    ctxAt_ = 0, ctxFrames_ = 0;
    float       ctxBpm_ = 0.0f;
    std::string ctxWhy_;
    std::atomic<bool> ctxPending_{false};
    std::atomic<bool> ctxSolo_{false};            ///< SOLO: the stem is neither read nor heard
    std::atomic<ContextState> ctxState_{ContextState::None};
    std::atomic<bool> ctxApart_{false};           ///< contextApart() as the last rebuild or publish found it
    /* Waveforms of the project in peaksDir_, by clip name, and the ones asked
     * for and not had yet, oldest first; under peaksMutex_. */
    std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> peaks_;
    std::vector<std::string> peaksWanted_;
    std::set<std::string> peaksMissing_;   ///< clips whose file is not in clips/
    std::string peaksDir_;
    std::mutex  peaksMutex_;
    std::atomic<uint32_t> peaksGen_{0};
    std::atomic<uint32_t> snapGen_{0};             // each rebuild(): the seq map follows it
    uint32_t mapSentGen_ = 0;                      // worker: the snap the sequencer was given
    std::atomic<uint8_t>  listReq_{0};
    std::atomic<uint32_t> listGen_{0};
    std::atomic<uint32_t> openGen_{0};    ///< openGeneration(): a song read in by doOpen()
    std::vector<std::string> projects_;
    std::vector<ClipInfo>    clips_;
    std::vector<ElementInfo> elements_;
    std::map<std::string, std::vector<ElementUse>> uses_;   ///< by element ref; under listMutex_
    std::set<std::string> snapshots_;                       ///< refs with a snapshot on the card; under listMutex_
    std::atomic<uint64_t> editHash_{0};
    /* requestUses() calls made, and how many of them the last finished scan
     * answers: the ones counted before it began reading the card. */
    std::atomic<uint32_t> usesAsked_{0}, usesAnswered_{0};
    std::mutex  listMutex_;
    Arrangement snap_;
    std::mutex  snapMutex_;
    /* Live clips (the clip launcher): what each track loops now, and what
     * was launched and is waiting for the next quant line. Under liveMutex_
     * -- the worker applies a request when the position reaches a line, the
     * UI launches and asks. The clip's path is resolved where the launch is
     * made, so the worker never needs songMutex_ while holding this one. */
    struct LiveSlot { std::string path, name; uint32_t loop = 0, startAt = 0; uint16_t bars = 0; };
    struct LiveReq  { std::string path, name; uint16_t bars = 0; bool stop = false, set = false; };
    std::mutex            liveMutex_;
    LiveSlot              liveCur_[kTracks];
    LiveReq               liveReq_[kTracks];
    std::atomic<uint32_t> liveSpb_{0};      // the grid's bar, samples
    std::atomic<uint32_t> liveCycle_{0};    // where the position wraps
    std::atomic<bool>     liveOn_{false};
    std::atomic<bool>     liveStartReq_{false};
    std::atomic<uint8_t>  liveQuant_{1};
    // Playback.
    FrameRing   ring_;
    ClipReader  readers_[kTracks];
    uint8_t*    ahead_ = nullptr;                 // the readers' read-ahead while the song plays, kAheadBytes each
    /// Where each track's blocks fall in the chunk being filled -- a block is
    /// at least a bar, so two at most, and a third for safety. The path is
    /// copied into the piece rather than a string: it is filled every 20 ms,
    /// and small heap blocks come from internal RAM. A member, not on the
    /// worker's stack: 184 B x 3 a track.
    struct Piece { char path[160]; uint32_t at, len, clipFrame; uint64_t step; };
    Piece       pieces_[kTracks][3];
    ClipReader  ctxReader_;                       // the context stem's: one descriptor beside the tracks'
    uint32_t    writePos_ = 0;
    std::atomic<uint32_t> debt_{0};
    std::atomic<float>    gain_[kTracks] = {};
    std::atomic<float>    pan_[kTracks] = {};     // -1..+1, with gain_: an audio track's balance
    /// One hop of the ring's meters: which hop it holds, and each track's peak.
    struct Meter {
        std::atomic<uint32_t> hop{UINT32_MAX};
        std::atomic<uint16_t> peak[kTracks][2] = {};
    };
    Meter                 meter_[kMeterSlots];
    std::atomic<float>    padsPeak_[2] = {};
    std::atomic<int8_t>   seqRoute_[kTracks];   ///< AudioSong::seqRoute(), per track; -1 from the constructor
    std::atomic<Play>     play_{Play::Idle};
    std::atomic<bool>     syncing_{false};         // this playback runs the sequencer
    uint32_t              startPos_ = 0;           // song sample playback started at
    std::atomic<uint32_t> armPos_{0};              // clock position it was started at
    std::atomic<uint32_t> pos_{0}, end_{0}, spBar_{0}, underruns_{0}, openFailures_{0};
    std::atomic<uint32_t> rtGen_{0};               // odd while the render is inside
    std::atomic<uint32_t> played_{0};              // frames sounded since this playback started
    // Count-in: asked for with a play, planned by the worker, played by RT
    // (a song alone) or by the metronome (in sync).
    static constexpr float kClickLevel = 0.35f;    // MetronomeNode's default level
    std::atomic<uint8_t>  countInReq_{0}, cinPlanned_{0}, cinBeatsLeft_{0};
    uint32_t              cinTotal_ = 0, cinLeft_ = 0, cinBeat_ = 1;   // samples; set before Counting
    uint8_t               cinBpb_ = 4;
    uint32_t              clickLen_ = 960, clickLeft_ = 0;
    float                 phase_ = 0.0f, step_ = 0.0f;
    std::atomic<bool>     loop_{true};
    std::atomic<uint32_t> loopA_{0}, loopB_{0};    // the loop region, samples
    std::atomic<bool>     stopReq_{false};
    std::vector<int16_t>  scratch_;
    int16_t*              io_ = nullptr;           // worker's card buffer, PSRAM
    int16_t*              vsrc_ = nullptr;         // worker's varispeed source frames (readAt()), PSRAM
    int32_t*              acc_ = nullptr;          // worker's mix-down sum, PSRAM
    uint32_t              sampleRate_ = 44100;
    // Bounce.
    FrameRing             bRing_;
    std::vector<int16_t>  capture_;
    std::atomic<bool>     bArmed_{false}, bDone_{false}, bOverflow_{false}, bCancel_{false};
    bool                  bCapturing_ = false;     // RT only
    uint32_t              bLeft_ = 0;              // RT only while armed
    uint8_t               bScene_ = 0, bTrack_ = 0;
    uint32_t              bBars_ = 0;                // 0 = one pass of the scene
    uint32_t              bFrom_ = 0;                // the song's bar the take starts on
    bool                  bFreeze_ = false;          // a freeze (freezeTrack()), not a bounce
    bool                  bToAudio_ = false;         // ... whose blocks become the track's audio (bounceSeqTrack())
    uint32_t              bFreezeKey_ = 0;           // seqKey() when it was asked for
    /* runFreeze()'s piece for runBounce(): the window's notes, the frames
     * recorded and the run-in before them; what the piece made. */
    std::vector<PatternSequencer::SongSpan> bWinSpans_;
    uint32_t              bWinTotal_ = 0, bWinSkip_ = 0;
    uint32_t              bSkip_ = 0;                // RT only while armed: run-in frames left
    bool                  bPieceOk_ = false;
    bool                  bGaveWay_ = false;         // the piece stopped for a player on its engine
    std::string           bPieceFile_;
    float                 bBpm_ = 120.0f;
    std::atomic<bool>     busy_{false};
    BounceStatus          status_;
    std::mutex            statusMutex_;
    std::atomic<Job>      job_{Job::None};
    std::atomic<bool>     bounceReq_{false};
    std::atomic<bool>     releaseReq_{false};
    std::mutex            workerMutex_;
    std::atomic<bool>     workerStarted_{false};
    std::atomic<bool>     quit_{false};
    // Stretch: the copies made (or found) on the card, and the blocks that
    // need none or cannot have one -- by path, cleared when a project opens.
    mutable std::mutex    stretchMutex_;
    std::set<std::string> stretchReady_, stretchSkip_;
    std::atomic<uint8_t>  stretchProg_{0};   ///< the block being fitted, 0..100
    // Undo: steps back and forward, and the arrangement as last seen (all
    // under songMutex_); the counts for the screen.
    std::vector<Snapshot> undo_, redo_;
    Snapshot              last_;
    int64_t               lastChangeUs_ = 0;
    std::atomic<uint32_t> undoCount_{0}, redoCount_{0};
};

} // namespace crosspad

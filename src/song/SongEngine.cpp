// SPDX-License-Identifier: MIT
//
// The audio song engine: projects in the platform's songs root, bouncing a
// sequencer scene to a WAV, and streaming four tracks of clips back into the
// mixer.
//
// Three threads touch it and each has one job:
//   render     takes the pads-only bus into the bounce ring, and plays the
//              song ring out (mix()). Never blocks, never touches storage.
//   worker     writes the bounce ring to storage, and keeps the song ring
//              filled -- the four tracks read and mixed down, in song time.
//   UI         edits the song, asks for bounces and playback.
// The rings between them are single-producer/single-consumer, so neither side
// waits on the other; storage work yields to whatever else the platform must
// read in time (ISongEnginePlatform::yieldIo).
//
// Everything storage does -- open, list, save, play, stop, bounce -- is a
// request to the worker; no public call waits for it. Only the worker
// allocates and frees the rings, and before it frees one it waits for the
// render to leave mix()/rtCapture() (rtGen_), so a Play pressed right after
// Stop can never hand the render a ring that is being freed.

#include "crosspad/song/SongEngine.hpp"

#include "crosspad/audio/WavWriter.hpp"
#include "crosspad/dsp/TimeStretch.hpp"
#include "crosspad/instrument/WavPcm.hpp"
#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/kit/KitInfo.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sequencer/MetronomeNode.hpp"
#include "crosspad/sequencer/PatternSequencer.hpp"
#include "crosspad/song/LiveSet.hpp"

#ifdef _WIN32
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <io.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

static_assert(AudioSong::kSeqPpq == PatternSequencer::kPpq,
              "a song's seq notes and a scene's events count the same ticks");
namespace {

constexpr int      kTracks           = AudioSong::kTracks;
constexpr uint32_t kBounceRingFrames = 32768;  // 128 kB, ~0.7 s of card stall
constexpr uint32_t kBounceMinFrames  = 8192;   // 32 kB (~170 ms), the least a bounce starts with
/* The tracks are mixed by the worker into one ring, not played from a ring
 * each: four rings were 128 kB of PSRAM, more than the board has free with an
 * app open, and play simply did not start. The price is that a mute or a
 * track fader is heard a ring's depth late.
 *
 * ~370 ms ahead, not the ~170 it was: moving round the UI while a song played
 * emptied it 21 times in three seconds (08.10.2026) -- the worker was off the
 * card longer than that. The ring takes what memory gives down to the prime,
 * so a board short of PSRAM plays as it did; the prime, and with it the wait
 * before the first note, is the same 4096 frames as before. */
constexpr uint32_t kSongRingFrames   = 16384;  // 64 kB, ~370 ms ahead
constexpr uint32_t kChunkFrames      = 1024;   // 4 kB per card access
constexpr uint32_t kPrimeFrames      = 4096;
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
/// Under clips/: a stretched track's blocks, fitted to the song's tempo.
constexpr const char* kStretchDir    = ".stretch";
/// Within this of 1:1 a block plays as recorded -- a copy would be the same
/// sound, a card read later.
constexpr double      kStretchSame   = 0.005;

/// A name that stays one folder under the songs root.
bool valid_name(const std::string& n)
{
    return !n.empty() && n.size() <= kMaxNameLen && n != "." && n != ".." &&
           n.find_first_of("/\\") == std::string::npos;
}

/// The few file operations the engine needs: POSIX where there is POSIX (the
/// board, Linux, macOS), the CRT and std::filesystem on Windows. Paths are
/// UTF-8.
namespace fsx {
#ifdef _WIN32
std::filesystem::path native(const std::string& p) { return std::filesystem::u8path(p); }
int  openRead(const std::string& p) { return ::_wopen(native(p).c_str(), _O_RDONLY | _O_BINARY); }
long readSome(int fd, void* dst, size_t n) { return ::_read(fd, dst, (unsigned)n); }
bool seek(int fd, uint32_t at) { return ::_lseeki64(fd, at, SEEK_SET) >= 0; }
void close(int fd) { ::_close(fd); }
bool exists(const std::string& p) { std::error_code ec; return std::filesystem::exists(native(p), ec); }
/// @p p's size, and when it was last written in unix seconds.
bool fileInfo(const std::string& p, uint64_t& bytes, int64_t& modified)
{
    std::error_code ec;
    bytes = std::filesystem::file_size(native(p), ec);
    if (ec) return false;
    const auto t = std::filesystem::last_write_time(native(p), ec);
    if (ec) return false;
    const auto sys = t - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now();
    modified = std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
    return true;
}
bool rename(const std::string& from, const std::string& to)
{
    std::error_code ec;
    std::filesystem::rename(native(from), native(to), ec);
    return !ec;
}
/// @p dir and everything in it.
template <typename Y> bool removeTree(const std::string& dir, Y)
{
    std::error_code ec;
    std::filesystem::remove_all(native(dir), ec);
    return !ec;
}
bool makeDir(const std::string& p)
{
    std::error_code ec;
    if (std::filesystem::is_directory(native(p), ec)) return true;
    return std::filesystem::create_directory(native(p), ec);
}
/// @p each(name, isDir) for every entry of @p dir.
template <typename F> void listDir(const std::string& dir, F each)
{
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(native(dir), ec)) {
        const auto u8 = e.path().filename().u8string();
        std::error_code dec;
        each(std::string(u8.begin(), u8.end()), e.is_directory(dec));
    }
}
#else
int  openRead(const std::string& p) { return ::open(p.c_str(), O_RDONLY); }
long readSome(int fd, void* dst, size_t n) { return (long)::read(fd, dst, n); }
bool seek(int fd, uint32_t at) { return ::lseek(fd, (off_t)at, SEEK_SET) >= 0; }
void close(int fd) { ::close(fd); }
bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }
/// @p p's size, and when it was last written in unix seconds.
bool fileInfo(const std::string& p, uint64_t& bytes, int64_t& modified)
{
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return false;
    bytes = (uint64_t)st.st_size;
    modified = (int64_t)st.st_mtime;
    return true;
}
bool rename(const std::string& from, const std::string& to) { return std::rename(from.c_str(), to.c_str()) == 0; }
bool makeDir(const std::string& p)
{
    struct stat st;
    if (stat(p.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(p.c_str(), 0775) == 0;
}
/// @p each(name, isDir) for every entry of @p dir.
template <typename F> void listDir(const std::string& dir, F each)
{
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        const std::string path = dir + "/" + e->d_name;
        struct stat st;
        each(std::string(e->d_name), stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
    }
    closedir(d);
}
/// @p dir and everything in it, @p yield() after each file. The names are
/// read before anything goes: removing while a directory is open for
/// reading is not something every filesystem survives.
template <typename Y> bool removeTree(const std::string& dir, Y yield)
{
    std::vector<std::pair<std::string, bool>> entries;
    listDir(dir, [&](const std::string& name, bool isDir) {
        if (name != "." && name != "..") entries.emplace_back(name, isDir);
    });
    bool ok = true;
    for (const auto& e : entries) {
        const std::string path = dir + "/" + e.first;
        if (e.second) ok = removeTree(path, yield) && ok;
        else          ok = ::unlink(path.c_str()) == 0 && ok;
        yield();
    }
    return ::rmdir(dir.c_str()) == 0 && ok;
}
#endif
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
    static constexpr uint32_t kMaxSegs   = 16;
    int16_t* seg[kMaxSegs] = {};
    uint32_t segs = 0;
    uint32_t cap = 0;
    std::atomic<uint32_t> head{0}, tail{0};

    static constexpr size_t kSegBytes = kSegFrames * 4;

    /// As many segments as memory gives, up to @p frames, at least @p
    /// minFrames.
    bool alloc(uint32_t frames, uint32_t minFrames) {
        release();
        const uint32_t want = std::min(kMaxSegs, (frames + kSegFrames - 1) / kSegFrames);
        const uint32_t need = (minFrames + kSegFrames - 1) / kSegFrames;
        while (segs < want) {
            void* p = platform->allocRingSegment(kSegBytes, segs < need ? need - segs : 1);
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

    void close() { if (fd >= 0) fsx::close(fd); fd = -1; path.clear(); cursor = UINT32_MAX; }

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
        uint32_t got = 0;
        if (fd >= 0 && frame < frames) {
            if (cursor != frame) {
                fsx::seek(fd, dataOffset + frame * channels * 2u);
                cursor = frame;
            }
            const uint32_t want = std::min(n, frames - frame);
            const uint32_t fb = channels * 2u;
            const long r = fsx::readSome(fd, dst, want * fb);
            got = r > 0 ? (uint32_t)r / fb : 0;
            if (channels == 1) {
                for (int32_t i = (int32_t)got - 1; i >= 0; --i) dst[i * 2] = dst[i * 2 + 1] = dst[i];
            }
            cursor = r > 0 && (uint32_t)r == got * fb ? cursor + got : UINT32_MAX;
        }
        if (got < n) std::memset(dst + got * 2, 0, (n - got) * 4);
        return n;
    }
};

/// What playback plays: the song flattened to sample ranges, rebuilt on
/// every commit. The worker reads it under snapMutex_; RT never does.
/// A block as samples of the song, and where in its file it starts.
struct Segment { std::string path; uint32_t start = 0, end = 0, offset = 0; };
struct Arrangement {
    std::vector<Segment> tracks[kTracks];
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

} // namespace

struct SongEngine::Impl {
    Impl(ISongEnginePlatform& platform, MetronomeNode& clock, PatternSequencer& sequencer)
        : p(platform), clk_(clock), seq_(sequencer) {
        ring_.platform = &p;
        bRing_.platform = &p;
    }

    ISongEnginePlatform& p;
    MetronomeNode& clk_;
    PatternSequencer& seq_;

    /// printf into the platform's log.
    void logf(bool warning, const char* fmt, ...) {
        char msg[192];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        p.log(warning, msg);
    }

    /// The worker is told to quit and joined. Only a platform that destroys
    /// the engine (PC, tests) gets here.
    void shutdown() {
        if (!workerStarted_.load()) return;
        quit_.store(true);
        p.wake();
        p.joinWorker();
    }

    // ── IAudioNode (render) ───────────────────────────────────────────────
    void onPrepare(uint32_t maxFrames, uint32_t sampleRate) {
        sampleRate_ = sampleRate ? sampleRate : 44100;
        scratch_.assign(maxFrames * 2, 0);
        capture_.assign(maxFrames * 2, 0);
    }
    void onSampleRateChange(uint32_t sr) { sampleRate_ = sr ? sr : 44100; }

    void mix(float* out, uint32_t frames) {
        RtSection rt(rtGen_);
        Play st = play_.load(std::memory_order_seq_cst);
        uint32_t skip = 0;             // frames of this block before the song's first
        if (st == Play::Counting) {
            skip = countIn(out, frames);
            if (skip == frames) return;
        } else if (st == Play::Armed) {
            /* The clock is mixed before this node, so the block in which its
             * position first moves off where it was started is the block it
             * started in, and how far it moved says where in the block: a
             * count-in hands over mid-block, and the song starts on its sample. */
            MetronomeNode& clk = clk_;
            const uint32_t ap = armPos_.load(std::memory_order_relaxed);
            const uint32_t cp = clk.positionSamples();
            if (!clk.running() || clk.countingIn() || cp == ap) return;
            const uint32_t loop = clk.loopSamples();
            const uint32_t moved = loop ? (cp + loop - ap) % loop : cp - ap;
            skip = moved < frames ? frames - moved : 0;
        }
        if (st == Play::Counting || st == Play::Armed) {
            /* Only from where it was: a stop the worker published since this
             * block began must not be turned back into Running. */
            Play expected = st;
            if (!play_.compare_exchange_strong(expected, Play::Running, std::memory_order_seq_cst)) return;
            st = Play::Running;
        }
        if (st != Play::Running) return;
        if (frames * 2 > scratch_.size()) return;
        const uint32_t n = frames - skip;
        played_.fetch_add(n, std::memory_order_release);
        const uint32_t got = ring_.pop(scratch_.data(), n);
        {
            /* How close the card came to falling behind, for ASONG_DBG. */
            const uint32_t left = ring_.used();
            if (left < dbgRingMin_.load(std::memory_order_relaxed)) dbgRingMin_.store(left, std::memory_order_relaxed);
        }
        if (got < n) {
            /* The card fell behind. Play silence and tell the worker how far
             * to skip, so the song comes back in time, not late. */
            debt_.fetch_add(n - got, std::memory_order_relaxed);
            underruns_.fetch_add(1, std::memory_order_relaxed);
        }
        const int16_t* s = scratch_.data();
        float* o = out + skip * 2;
        for (uint32_t i = 0; i < got * 2; ++i) o[i] += (float)s[i] * (1.0f / 32768.0f);
        uint32_t pos = pos_.load(std::memory_order_relaxed) + n;
        const uint32_t end = end_.load(std::memory_order_relaxed);
        const bool loop = loop_.load(std::memory_order_relaxed);
        const uint32_t la = loopA_.load(std::memory_order_relaxed), lb = loopB_.load(std::memory_order_relaxed);
        const uint32_t lim = loop && lb > la ? lb : end;
        if (lim && pos >= lim) {
            /* Modulo, not minus: the song can be shortened while it plays. */
            if (loop) pos = wrap_position(pos, end, la, lb);
            else {
                Play expected = Play::Running;
                play_.compare_exchange_strong(expected, Play::Ended, std::memory_order_seq_cst);
            }
        }
        pos_.store(pos, std::memory_order_relaxed);
    }

    /// RT: the song's own count-in, the metronome's click (see MetronomeNode)
    /// on every beat, a fifth up on the downbeat. @return the frames of the
    /// block it took -- all of them until the last beat is out.
    uint32_t countIn(float* out, uint32_t frames) {
        const uint32_t spb = cinBeat_ ? cinBeat_ : 1;
        uint32_t left = cinLeft_;
        uint32_t i = 0;
        for (; i < frames && left; ++i, --left) {
            const uint32_t at = cinTotal_ - left;
            if (at % spb == 0) {
                clickLeft_ = clickLen_;
                const bool downbeat = (at / spb) % cinBpb_ == 0;
                step_ = (downbeat ? 1500.0f : 1000.0f) * 6.2831853f / (float)sampleRate_;
                phase_ = 0.0f;
            }
            if (clickLeft_) {
                const float env = (float)clickLeft_ / (float)clickLen_;
                const float v = std::sin(phase_) * env * env * kClickLevel;
                out[i * 2] += v;
                out[i * 2 + 1] += v;
                phase_ += step_;
                --clickLeft_;
            }
        }
        cinLeft_ = left;
        cinBeatsLeft_.store((uint8_t)((left + spb - 1) / spb), std::memory_order_relaxed);
        return i;
    }

    /// The pads bus, one block. Only while a bounce is armed.
    void rtCapture(const float* bus, uint32_t frames) {
        RtSection rt(rtGen_);
        /* The seq track's meter: its notes play through the pads, live. */
        float pl = 0.0f, pr = 0.0f;
        for (uint32_t i = 0; i < frames; ++i) {
            pl = std::max(pl, std::fabs(bus[i * 2]));
            pr = std::max(pr, std::fabs(bus[i * 2 + 1]));
        }
        if (pl > padsPeak_[0].load(std::memory_order_relaxed)) padsPeak_[0].store(pl, std::memory_order_relaxed);
        if (pr > padsPeak_[1].load(std::memory_order_relaxed)) padsPeak_[1].store(pr, std::memory_order_relaxed);
        if (!bArmed_.load(std::memory_order_seq_cst)) return;
        if (!bCapturing_) {
            /* Start on the block in which the clock started, so sample 0 of
             * the file is beat one of the scene. */
            MetronomeNode& clk = clk_;
            const uint32_t at = clk.positionSamples();
            if (!clk.running() || clk.countingIn() || at == 0 || at > frames) return;
            bCapturing_ = true;
        }
        const uint32_t n = std::min(frames, bLeft_);
        if (n * 2 > capture_.size()) { bOverflow_.store(true); return; }
        int16_t* dst = capture_.data();
        for (uint32_t i = 0; i < n * 2; ++i) {
            float v = bus[i] * 32767.0f;
            dst[i] = (int16_t)(v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
        }
        if (bRing_.push(dst, n) < n) bOverflow_.store(true);
        bLeft_ -= n;
        if (!bLeft_) {
            bCapturing_ = false;
            bArmed_.store(false, std::memory_order_release);
            bDone_.store(true, std::memory_order_release);
        }
    }

    /// The worker and its card buffer, on first use rather than at boot: boot
    /// is when PSRAM is tightest (the kit scan grows its list in one block),
    /// and a board that never opens a song should not pay for one.
    bool ensureWorker() {
        if (workerStarted_.load()) return true;
        std::lock_guard<std::mutex> l(workerMutex_);
        if (workerStarted_.load()) return true;
        /* The one card buffer the worker uses, for whichever job it is on,
         * and the sum the tracks are mixed into. */
        if (!io_) io_ = static_cast<int16_t*>(p.allocScratch(kChunkFrames * 4));
        if (!acc_) acc_ = static_cast<int32_t*>(p.allocScratch(kChunkFrames * 8));
        if (!io_ || !acc_) return false;
        workerStarted_.store(p.startWorker(worker_entry, this));
        return workerStarted_.load();
    }

    // ── Projects ──────────────────────────────────────────────────────────
    bool requestProjects() { return request(kListProjects); }
    std::vector<std::string> projects() {
        std::lock_guard<std::mutex> l(listMutex_);
        return projects_;
    }

    bool requestOpen(const std::string& name) {
        if (!valid_name(name) || !ensureWorker()) return false;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            openName_ = name;
        }
        openState_.store(SongOpState::Busy);
        openReq_.store(true);
        p.wake();
        return true;
    }
    SongOpState openState() const { return openState_.load(); }

    bool requestRename(const std::string& name) {
        if (!valid_name(name) || !ensureWorker()) return false;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            editName_ = name;
        }
        return requestEdit(kEditRename);
    }

    bool requestDelete() {
        if (!ensureWorker()) return false;
        return requestEdit(kEditDelete);
    }

    bool requestTempo(float bpm) {
        if (!(bpm >= 20.0f && bpm <= 400.0f) || !ensureWorker()) return false;
        editBpm_.store(bpm);
        return requestEdit(kEditTempo);
    }

    SongOpState editState() const { return editState_.load(); }

    bool requestMerge(uint8_t trackMask, uint32_t fromBar, uint32_t toBar) {
        const uint8_t all = (uint8_t)((1u << kTracks) - 1);
        if (!(trackMask & all) || toBar <= fromBar || !ensureWorker()) return false;
        editMask_.store(trackMask & all);
        editFrom_.store(fromBar);
        editTo_.store(toBar);
        editProgress_.store(0);
        return requestEdit(kEditMerge);
    }
    uint8_t editProgress() const { return editProgress_.load(); }
    uint32_t stretchWaiting() const { return stretchCount_.load(); }
    uint8_t stretchProgress() const { return stretchProg_.load(); }
    bool undo() { return step(undo_, redo_); }
    bool redo() { return step(redo_, undo_); }
    uint32_t undoSteps() const { return undoCount_.load(); }
    uint32_t redoSteps() const { return redoCount_.load(); }

    /// Track @p t's level now, 0..1 a side: an audio track's loudest frame
    /// in the last few hops RT has played (its gain applied), the seq
    /// track's the pads' since the last ask. 0 while stopped.
    void trackLevels(int t, float& left, float& right) {
        left = right = 0.0f;
        if (t < 0 || t >= kTracks) return;
        const bool seq = t == seqTrack_.load(std::memory_order_relaxed);
        const float pl = seq ? padsPeak_[0].exchange(0.0f, std::memory_order_relaxed) : 0.0f;
        const float pr = seq ? padsPeak_[1].exchange(0.0f, std::memory_order_relaxed) : 0.0f;
        if (play_.load(std::memory_order_relaxed) != Play::Running) return;
        if (seq) {
            left = std::min(1.0f, pl);
            right = std::min(1.0f, pr);
            return;
        }
        const uint32_t hop = ring_.tail.load(std::memory_order_acquire) / kMeterHop;
        uint32_t m[2] = {0, 0};
        for (uint32_t back = 0; back < kMeterRead && back <= hop; ++back) {
            const Meter& s = meter_[(hop - back) % kMeterSlots];
            if (s.hop.load(std::memory_order_acquire) != hop - back) continue;
            for (int c = 0; c < 2; ++c) m[c] = std::max<uint32_t>(m[c], s.peak[t][c].load(std::memory_order_relaxed));
        }
        left = (float)m[0] / 32767.0f;
        right = (float)m[1] / 32767.0f;
    }

    bool requestExportLiveSet() {
        if (!ensureWorker()) return false;
        return requestEdit(kEditExport);
    }
    std::vector<std::string> editReport() {
        std::lock_guard<std::mutex> l(reportMutex_);
        return report_;
    }

    bool clipPeaks(const std::string& file, ClipPeaks& out) {
        std::string dir;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            dir = song_.dir;
        }
        if (dir.empty() || file.empty()) return false;
        {
            std::lock_guard<std::mutex> l(peaksMutex_);
            if (dir != peaksDir_) {           // another project: none of these are its
                peaks_.clear();
                peaksWanted_.clear();
                peaksMissing_.clear();
                peaksDir_ = dir;
            }
            auto it = peaks_.find(file);
            if (it != peaks_.end()) {
                out.hop = kPeakHop;
                out.values = it->second;
                out.missing = peaksMissing_.count(file) != 0;
                return true;
            }
            if (std::find(peaksWanted_.begin(), peaksWanted_.end(), file) != peaksWanted_.end()) return false;
            peaksWanted_.push_back(file);
        }
        if (ensureWorker()) p.wake();
        return false;
    }
    uint32_t peaksGeneration() const { return peaksGen_.load(); }

    /// Why a playing song can be silent: the published gains the worker
    /// gates on, how many segments flatten() gave each track, where the
    /// write head is against the end, and what the ring holds.
    const char* debugLine() {
        uint32_t end = 0, la = 0, lb = 0;
        int seg[kTracks] = {};
        {
            std::lock_guard<std::mutex> l(snapMutex_);
            end = snap_.end; la = snap_.loopA; lb = snap_.loopB;
            for (int t = 0; t < kTracks; ++t) seg[t] = (int)snap_.tracks[t].size();
        }
        /* The stream's margins since the last read, read and cleared: the
         * longest the worker was off the ring, its longest card access and
         * song.json write, and the least the ring held. */
        const uint32_t ringMin = dbgRingMin_.exchange(UINT32_MAX);
        std::snprintf(dbg_, sizeof(dbg_),
                      "g=%.2f/%.2f/%.2f/%.2f seg=%d/%d/%d/%d end=%u loop=%u-%u "
                      "wp=%u ringfill=%u ringcap=%u ringmin=%u gap_max_us=%u read_max_us=%u "
                      "save_max_us=%u play=%d job=%d live=%d",
                      (double)gain_[0].load(), (double)gain_[1].load(),
                      (double)gain_[2].load(), (double)gain_[3].load(),
                      seg[0], seg[1], seg[2], seg[3],
                      (unsigned)end, (unsigned)la, (unsigned)lb,
                      (unsigned)writePos_, (unsigned)ring_.used(), (unsigned)ring_.cap,
                      ringMin == UINT32_MAX ? 0u : (unsigned)ringMin,
                      (unsigned)dbgGapMaxUs_.exchange(0), (unsigned)dbgReadMaxUs_.exchange(0),
                      (unsigned)dbgSaveMaxUs_.exchange(0),
                      (int)play_.load(), (int)job_.load(), (int)liveOn_.load());
        return dbg_;
    }
    char dbg_[320] = {};
    /* ASONG_DBG's margins: see debugLine(). */
    std::atomic<uint32_t> dbgRingMin_{UINT32_MAX};
    std::atomic<uint32_t> dbgGapMaxUs_{0};
    std::atomic<uint32_t> dbgReadMaxUs_{0};
    std::atomic<uint32_t> dbgSaveMaxUs_{0};
    int64_t lastFillUs_ = 0;                       // worker: the last top-up, 0 while not streaming
    static void dbgMax(std::atomic<uint32_t>& a, int64_t v) {
        const uint32_t u = v > 0 ? (uint32_t)std::min<int64_t>(v, UINT32_MAX) : 0;
        if (u > a.load(std::memory_order_relaxed)) a.store(u, std::memory_order_relaxed);
    }
    uint32_t framesPerBar() {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        return song_.samplesPerBar(sampleRate_);
    }

    /// Where audio meant for @p track may go (UX 2000 section 7.1): that
    /// track if it is an audio track, the next free one if nobody has made
    /// it yet -- tracks are made from the lowest free one, never with a hole
    /// below -- and nowhere (-1) if it is the seq track or all four are taken.
    /// @p make: also make the free one. Caller holds songMutex_.
    int audioTarget(uint8_t track, bool make) {
        if (track >= kTracks) return -1;
        SongTrack& t = song_.tracks[track];
        /* A track holding clips is in use whatever its flag says: songs from
         * before tracks had kinds are exactly that. */
        if (t.inUse || !t.clips.empty()) {
            if (t.kind != TrackKind::Audio) return -1;
            if (make) t.inUse = true;
            return track;
        }
        const int f = song_.freeTrack();
        if (f >= 0 && make) song_.addAudioTrack(f);
        return f;
    }

    bool requestImport(const std::string& file, uint32_t start, uint32_t end,
                       uint8_t track, uint32_t bar, uint32_t lead) {
        if (file.empty() || track >= kTracks || (end && end <= start) || !ensureWorker()) return false;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            if (audioTarget(track, false) < 0) return false;
            editName_ = file;
        }
        importA_ = start;
        importB_ = end;
        importTrack_ = track;
        importBar_ = bar;
        importLead_ = lead;
        editProgress_.store(0);
        return requestEdit(kEditImport);
    }

    /// The first look at the song is what opens the last project.
    AudioSong& song() {
        if (!opened_ && ensureWorker()) p.wake();
        return song_;
    }
    std::recursive_mutex& songMutex() { return songMutex_; }

    void commit() {
        if (!ensureWorker()) return;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            rebuild();
        }
        saveAtUs_.store(p.nowUs() + (int64_t)kSaveDelayMs * 1000);
        saveReq_.store(true);
        p.wake();
    }

    bool requestClips() { return request(kListClips); }
    std::vector<ClipInfo> clips() {
        std::lock_guard<std::mutex> l(listMutex_);
        return clips_;
    }
    uint32_t listGeneration() const { return listGen_.load(); }

    // ── Bounce ────────────────────────────────────────────────────────────
    uint32_t playEndBars() {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        return endBars(song_);
    }

    bool bounceSeqTrack(uint8_t track) {
        if (track >= kTracks) return false;
        uint8_t scene = 0;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            const SongTrack& t = song_.tracks[track];
            if (!t.inUse || t.kind != TrackKind::Seq) return false;
            scene = t.scene;
        }
        return requestBounce(scene, track, playEndBars());
    }

    bool bounceScene(uint8_t scene, uint8_t track) { return requestBounce(scene, track, 0); }

    /// @p bars 0: one pass of the scene; otherwise that many bars, the scene
    /// looping over them, onto @p track from bar one.
    bool requestBounce(uint8_t scene, uint8_t track, uint32_t bars) {
        if (track >= kTracks || !ensureWorker()) return false;
        /* A scene's bounce is audio for an audio track; only a seq track's own
         * bounce lands on the seq track. */
        if (!bars) {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            if (audioTarget(track, false) < 0) return false;
        }
        if (busy_.exchange(true)) return false;
        {
            std::lock_guard<std::mutex> l(statusMutex_);
            status_ = BounceStatus{};
            status_.state = BounceState::Running;
        }
        bScene_ = scene;
        bTrack_ = track;
        bBars_ = bars;
        bCancel_.store(false);
        bounceReq_.store(true);
        p.wake();
        return true;
    }

    BounceStatus bounceStatus() {
        std::lock_guard<std::mutex> l(statusMutex_);
        return status_;
    }

    void cancelBounce() { bCancel_.store(true); }

    // ── Playback ──────────────────────────────────────────────────────────
    bool playCountedIn(uint32_t fromBar, SyncTake take, uint8_t beats) {
        countInReq_.store(beats);
        if (playWithTake(fromBar, take)) return true;
        countInReq_.store(0);
        return false;
    }

    uint8_t countInBeatsLeft() const {
        switch (play_.load()) {
            case Play::Priming:  return cinPlanned_.load();
            case Play::Counting: return cinBeatsLeft_.load(std::memory_order_relaxed);
            case Play::Armed:    return syncing_.load() ? clk_.countInBeatsLeft() : 0;
            default:             return 0;
        }
    }

    bool playWithTake(uint32_t fromBar, SyncTake take) {
        if (!ensureWorker() || busy_.load()) return false;           // a bounce owns the transport
        /* A seq track's length is its scene's, which the seq view changes
         * without a commit: taken again from a stop, not from the last one. */
        if (!playing()) {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            rebuild();
        }
        {
            std::lock_guard<std::mutex> l(snapMutex_);
            if (!snap_.end && !snap_.loopB) return false;
        }
        playFrom_.store(fromBar);
        playTake_.store(take);
        stopReq_.store(false);
        playReq_.store(true);
        p.wake();
        return true;
    }

    void stop() {
        if (!workerStarted_.load()) return;
        playReq_.store(false);
        stopReq_.store(true);
        p.wake();
    }

    bool playing() const {
        const Play p = play_.load();
        return p == Play::Priming || p == Play::Armed || p == Play::Counting || p == Play::Running;
    }

    double positionBars() const {
        const uint32_t spb = spBar_.load();
        return spb ? (double)pos_.load() / spb : 0.0;
    }

    uint32_t framesPlayed() const { return played_.load(std::memory_order_acquire); }
    uint32_t positionFrames() const { return pos_.load(std::memory_order_relaxed); }

    uint32_t underruns() const { return underruns_.load(); }
    uint32_t openFailures() const { return openFailures_.load(); }

    // ── Takes onto the seq track (Arrange's REC and DUB) ──────────────────
    bool recordSeq(uint32_t fromBar, SyncTake take, uint8_t beats) {
        if (take == SyncTake::None || playing() || !ensureWorker() || busy_.load()) return false;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            if (!opened_ || seqTake_.load() != SyncTake::None) return false;
            if (!openTake(take)) return false;
            /* Round a region, from its start: the passes are its laps. */
            if (takeCycle_) fromBar = song_.loopStart;
            takeFrom_ = fromBar * song_.samplesPerBar(sampleRate_);
        }
        if (playCountedIn(fromBar, SyncTake::None, beats)) return true;
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        closeTake();
        return false;
    }

    bool seqPunch(SyncTake take) {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        const SyncTake now = seqTake_.load();
        if (take == now) return true;
        /* Into a song playing with the sequencer locked to it: that is
         * what keeps the hits and counts where the song is. */
        if (take != SyncTake::None && !seqPunchable()) return false;
        if (now != SyncTake::None) {
            finishTake();
            takeBound_ = false;
        }
        if (take == SyncTake::None) return true;
        if (!openTake(take)) return false;
        takeFrom_ = seq_.songMapPosition();
        takeBound_ = true;
        return true;
    }

    SyncTake seqTake() const { return seqTake_.load(); }

    // ── The seq view of a block ───────────────────────────────────────────
    bool openSeqEdit(uint8_t track, uint32_t bar, uint8_t scene) {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        closeSeqEdit();
        if (track >= kTracks || scene >= PatternSequencer::kNumSequences) return false;
        SongTrack& tr = song_.tracks[track];
        if (!tr.inUse || tr.kind != TrackKind::Seq) return false;
        /* Read before it is written over: the scene may be the very one
         * the track's older blocks play. */
        const int adopted = song_.adoptSceneNotes(track, scenePattern(tr.scene), endBars(song_));
        tr.seqBlocks = true;
        const int i = blockAt(tr, bar);
        SeqPatternPtr notes = i >= 0 ? tr.clips[(size_t)i].notes : nullptr;
        if (notes && (notes->bars > PatternSequencer::kMaxBars || notes->notes.size() > PatternSequencer::kMaxEvents)) {
            if (adopted) commit();
            return false;
        }
        seq_.clearSeq(scene, 127);
        seq_.setSceneBars(scene, notes ? (uint8_t)notes->bars : PatternSequencer::kDefaultBars);
        if (notes) {
            for (const SeqNote& n : notes->notes)
                seq_.addEventTicks(scene, n.tick, n.pad, n.vel, n.len, PadPerformance::unpack(n.perf));
        }
        editTrack_ = track;
        editBar_ = i >= 0 ? tr.clips[(size_t)i].bar : bar;
        editScene_ = scene;
        editBound_ = i >= 0;
        editGen_ = seq_.editGeneration();
        if (adopted) commit();
        return true;
    }

    void syncSeqEdit() {
        if (editTrack_ < 0 || seq_.editGeneration() == editGen_ || seq_.isRecording()) return;
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (editTrack_ < 0) return;
        editGen_ = seq_.editGeneration();
        const int track = editTrack_.load();
        SongTrack& tr = song_.tracks[track];
        if (!tr.inUse || tr.kind != TrackKind::Seq) { editTrack_ = -1; return; }
        const SeqPatternPtr now = scenePattern(editScene_);
        int i = -1;
        for (size_t k = 0; k < tr.clips.size() && i < 0; ++k)
            if (tr.clips[k].bar == editBar_) i = (int)k;
        if (i < 0) {
            /* The block went (deleted, undone) while it was being edited:
             * nothing is put back in its place. */
            if (editBound_ || now->notes.empty()) {
                if (editBound_) editTrack_ = -1;
                return;
            }
            /* The first notes of a new block: on its bar, as long as the
             * scene, never over the next block. */
            SongClip c;
            c.bar = editBar_;
            c.bars = std::min<uint32_t>(now->bars, roomAt(tr, editBar_));
            c.notes = now;
            song_.addClip(track, c);
            editBound_ = true;
            commit();
            return;
        }
        SongClip& c = tr.clips[(size_t)i];
        if (samePattern(c.notes, now)) return;
        /* A block as long as its notes stays so when the scene grows or
         * shrinks (x2, /2, its length): what the seq view shows is the block. */
        if (c.notes && c.bars == c.notes->bars)
            c.bars = (uint16_t)std::min<uint32_t>(now->bars, roomAt(tr, c.bar));
        c.notes = now;
        commit();
    }

    void closeSeqEdit() {
        syncSeqEdit();
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        editTrack_ = -1;
    }

    int32_t seqEditBar() const { return editTrack_ >= 0 ? (int32_t)editBar_ : -1; }

    bool seqPunchable() const {
        return play_.load() == Play::Running && syncing_.load() && seq_.state() == SequencerState::Running;
    }

    /// Stop playback, cancel any bounce and give both rings back, before the
    /// engine arena they may live in is wiped. Waits for song_io; never call
    /// it from song_io or while holding the sample player's locks, which the
    /// worker's card reads take.
    void releaseRings() {
        if (!workerStarted_.load()) return;
        bCancel_.store(true);
        playReq_.store(false);
        stopReq_.store(true);
        releaseReq_.store(true);
        p.wake();
        /* Until it is done, however long a card job takes: the caller wipes
         * the arena next, and a ring still in it would be written by RT into
         * memory the pitched bank owns, then freed as a foreign pointer. */
        for (uint32_t waited = 0; releaseReq_.load(); waited += kReleasePollMs) {
            p.sleepMs(kReleasePollMs);
            if (waited && waited % kReleaseWaitMs == 0) {
                logf(true, "rings still not released after %u ms (song_io busy on the card)",
                     (unsigned)waited);
            }
        }
    }

    // ── the clip launcher's live transport ───────────────────────────────
    bool liveStart() {
        if (!opened_.load() || !ensureWorker()) return false;
        liveStartReq_.store(true);
        p.wake();
        return true;
    }

    bool liveLaunch(uint8_t track, const std::string& file, uint16_t bars) {
        if (track >= kTracks || file.empty() || !opened_.load() || !ensureWorker()) return false;
        std::string path;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            path = song_.clipPath(file);
        }
        {
            std::lock_guard<std::mutex> l(liveMutex_);
            LiveReq& r = liveReq_[track];
            r.path = path;
            r.name = file;
            r.bars = bars ? bars : 1;
            r.stop = false;
            r.set  = true;
        }
        /* The first launch starts the transport: the request is already in,
         * so it lands on the first sample the grid plays. */
        if (!liveOn_.load()) liveStartReq_.store(true);
        p.wake();
        return true;
    }

    void liveQuant(uint8_t bars) { liveQuant_.store(bars ? bars : 1); }

    bool liveStop(uint8_t track) {
        if (!liveOn_.load()) return false;
        {
            std::lock_guard<std::mutex> l(liveMutex_);
            for (int t = 0; t < kTracks; ++t) {
                if (track < kTracks && t != (int)track) continue;
                liveReq_[t] = LiveReq{};
                liveReq_[t].stop = true;
                liveReq_[t].set  = true;
            }
        }
        p.wake();
        return true;
    }

    bool liveState(ISongEngine::LiveState& out) {
        out = ISongEngine::LiveState{};
        const uint32_t spb = liveSpb_.load();
        out.on = liveOn_.load();
        out.quantBars = liveQuant_.load();
        out.framesPerBar = spb;
        if (!out.on) return true;
        const uint32_t pos = pos_.load(std::memory_order_relaxed);
        out.bars = spb ? (double)pos / (double)spb : 0.0;
        std::lock_guard<std::mutex> l(liveMutex_);
        for (int t = 0; t < kTracks; ++t) {
            ISongEngine::LiveTrack& o = out.tracks[t];
            const LiveSlot& c = liveCur_[t];
            o.file       = c.name;
            o.bars       = c.bars;
            o.loopFrames = c.loop;
            /* The worker fills the ring ahead of what is heard, so a clip
             * applied a moment ago can start later than the audible
             * position: nothing into its loop yet, not a huge phase. */
            o.phase = c.loop && pos > c.startAt ? (pos - c.startAt) % c.loop : 0;
            if (liveReq_[t].set) {
                o.stopping = liveReq_[t].stop;
                o.pending  = liveReq_[t].stop ? std::string() : liveReq_[t].name;
            }
        }
        return true;
    }


private:
    /* Only playback is a standing job; a bounce is a request of its own, so a
     * stop or a play arriving beside it can never overwrite it. */
    enum class Job : uint8_t { None, Play };
    static constexpr uint8_t kListProjects = 1;
    static constexpr uint8_t kListClips    = 2;

    static void worker_entry(void* arg) { static_cast<Impl*>(arg)->worker(); }

    bool request(uint8_t list) {
        if (!ensureWorker()) return false;
        listReq_.fetch_or(list);
        p.wake();
        return true;
    }

    bool requestEdit(uint8_t edit) {
        editState_.store(SongOpState::Busy);
        editReq_.store(edit);
        p.wake();
        return true;
    }

    /// Worker: wait until a render that may have seen the old state has left
    /// mix()/rtCapture(). Call after publishing the state that keeps the next
    /// render out (play_ Idle, bArmed_ false).
    void waitRtOut() {
        const uint32_t g = rtGen_.load(std::memory_order_seq_cst);
        if (!(g & 1u)) return;
        while (rtGen_.load(std::memory_order_seq_cst) == g) p.sleepMs(1);
    }

    /// Worker: project folders under the songs root.
    void doListProjects() {
        std::vector<std::string> out;
        if (p.storageReady()) {
            const std::string root = p.songsRoot();
            fsx::listDir(root, [&](const std::string& name, bool isDir) {
                if (name.empty() || name[0] == '.') return;
                if (isDir) out.push_back(name);
                p.yieldIo();
            });
        }
        std::sort(out.begin(), out.end());
        std::lock_guard<std::mutex> l(listMutex_);
        projects_ = std::move(out);
    }

    /// Worker: the clips of the open project, each with its length on the
    /// grid. A file with no frames is a bounce that never closed; it is left
    /// out rather than offered as a clip.
    void doListClips() {
        std::vector<ClipInfo> out;
        std::string dir;
        uint32_t spBar = 0;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            if (opened_) {
                dir = song_.dir + "/" + AudioSong::clipsDir();
                spBar = song_.samplesPerBar(sampleRate_);
            }
        }
        if (!dir.empty() && p.storageReady()) {
            fsx::listDir(dir, [&](const std::string& name, bool) {
                    if (name.size() < 5 || name.compare(name.size() - 4, 4, ".wav") != 0) return;
                    /* Its length on the grid from the header alone: a bounce
                     * is a whole number of bars, anything else rounds up.
                     * Asked before every open: on the board an open that
                     * finds internal RAM short aborts rather than fails. */
                    ClipReader r;
                    if (p.storageReady() && r.open(dir + "/" + name) && r.frames && spBar) {
                        ClipInfo ci;
                        ci.file = name;
                        const uint32_t bars = (r.frames + spBar - 1) / spBar;
                        ci.bars = (uint16_t)std::max<uint32_t>(1, std::min<uint32_t>(bars, 999));
                        out.push_back(ci);
                    }
                    r.close();
                    p.yieldIo();
            });
        }
        std::sort(out.begin(), out.end(), [](const ClipInfo& a, const ClipInfo& b) { return a.file < b.file; });
        std::lock_guard<std::mutex> l(listMutex_);
        clips_ = std::move(out);
    }

    /// Worker: open a project -- make its folders, read its song, remember
    /// it. Read into a copy first, outside songMutex_, so the UI's timers are
    /// not held out by the card; a song that cannot be read is not opened, and
    /// the one open stays -- nothing then saves an empty song over it.
    SongOpState doOpen(const std::string& name) {
        if (!p.storageReady()) return SongOpState::Failed;
        const std::string root = p.songsRoot();
        const std::string dir = root + "/" + name;
        if (!fsx::makeDir(root) || !fsx::makeDir(dir) || !fsx::makeDir(dir + "/" + AudioSong::clipsDir())) {
            return SongOpState::Failed;
        }
        AudioSong fresh;
        if (fresh.load(dir) == SongLoad::Unreadable) {
            logf(true, "%s/%s unreadable; left as it is", dir.c_str(), AudioSong::fileName());
            return SongOpState::Failed;
        }
        fresh.name = name;
        endPlayback();
        {
            std::lock_guard<std::mutex> l(stretchMutex_);
            stretchReady_.clear();
            stretchSkip_.clear();
        }
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            song_ = std::move(fresh);
            /* Another song: nothing of the last one's to undo. */
            undo_.clear();
            redo_.clear();
            undoCount_.store(0);
            redoCount_.store(0);
            pack(song_, last_);
            lastChangeUs_ = 0;
            rebuild();
        }
        opened_ = true;
        p.saveCurrent(name);
        return SongOpState::Ready;
    }

    /// Worker: the project last open, once the card is there. Tried once per
    /// boot with a card present, whatever the answer: an unreadable song must
    /// not be read again every second.
    void openLast() {
        if (!p.storageReady()) return;
        lastTried_ = true;
        std::string name = p.loadCurrent();
        if (!valid_name(name)) name = "Song 1";
        openState_.store(doOpen(name));
    }

    /// Worker: the open project's folder under the name asked for, and that
    /// name remembered as the one to open next time.
    SongOpState doRename() {
        std::string name, from;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            name = editName_;
            from = song_.dir;
        }
        if (!opened_ || !p.storageReady()) return SongOpState::Failed;
        const std::string to = std::string(p.songsRoot()) + "/" + name;
        if (fsx::exists(to)) return SongOpState::Failed;              // taken
        endPlayback();
        if (!fsx::rename(from, to)) return SongOpState::Failed;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            song_.dir = to;
            song_.name = name;
            rebuild();
        }
        p.saveCurrent(name);
        return SongOpState::Ready;
    }

    /// Worker: the open project's folder gone, and another open in its place.
    /// Only ever a folder straight under the songs root, by a valid name: a
    /// song whose dir says anything else is refused rather than removed.
    SongOpState doDelete() {
        std::string name, dir;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            name = song_.name;
            dir = song_.dir;
        }
        if (!opened_ || !p.storageReady() || !valid_name(name) ||
            dir != std::string(p.songsRoot()) + "/" + name) {
            return SongOpState::Failed;
        }
        endPlayback();
        /* Nothing may save the song being removed back onto the card: until
         * another is open there is none. */
        opened_ = false;
        saveReq_.store(false);
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            song_ = AudioSong();
            rebuild();
        }
        const bool removed = fsx::removeTree(dir, [this] { p.yieldIo(); });
        if (!removed) logf(true, "%s: not all of it could be removed", dir.c_str());
        doListProjects();
        listGen_.fetch_add(1);
        std::string next = "Song 1";
        {
            std::lock_guard<std::mutex> l(listMutex_);
            if (!projects_.empty()) next = projects_.front();
        }
        openState_.store(doOpen(next));
        return removed ? SongOpState::Ready : SongOpState::Failed;
    }

    /// Worker: a new tempo, and every block as long as its file lasts at it.
    /// The headers are read into a copy outside songMutex_, so the UI's
    /// timers are not held out by the card.
    SongOpState doTempo(float bpm) {
        if (!opened_) return SongOpState::Failed;
        AudioSong copy;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            copy = song_;
        }
        copy.bpm = bpm;
        const uint32_t spBar = copy.samplesPerBar(sampleRate_);
        for (auto& tr : copy.tracks) {
            /* A stretched track keeps its blocks' bars: its audio is fitted
             * to them (doStretch), not they to it. */
            if (tr.stretch != StretchMode::Off && tr.kind == TrackKind::Audio) continue;
            for (auto& c : tr.clips) {
                /* A seq block is bars of the scene: it keeps its length. */
                if (c.file.empty()) continue;
                ClipReader r;
                if (!p.storageReady() || !r.open(copy.clipPath(c.file))) continue;   // missing: keep what it had
                /* What the block plays: from its offset, to a cut or the end. */
                const uint32_t rest = r.frames > c.offset ? r.frames - c.offset : 0;
                const uint32_t len = c.frames ? std::min(c.frames, rest) : rest;
                c.bars = (uint16_t)std::max<uint32_t>(1, std::min<uint32_t>((len + spBar - 1) / spBar, 999));
                r.close();
                p.yieldIo();
            }
        }
        endPlayback();
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            song_.bpm = bpm;
            /* Sizes matched to the blocks by place: an edit made while the
             * headers were read would have moved them, and wins. */
            for (int t = 0; t < kTracks; ++t) {
                auto& cs = song_.tracks[t].clips;
                const auto& nc = copy.tracks[t].clips;
                for (size_t i = 0; i < cs.size() && i < nc.size(); ++i) {
                    if (cs[i].file == nc[i].file && cs[i].bar == nc[i].bar && cs[i].offset == nc[i].offset)
                        cs[i].bars = nc[i].bars;
                }
            }
            rebuild();
        }
        doSave();
        return SongOpState::Ready;
    }

    /// Worker: the tracks in @p mask mixed down, [fromBar, toBar) and on to the
    /// end of any block that starts inside, into one clip on the lowest of
    /// them -- see ISongEngine::requestMerge. Read from a copy of the song,
    /// so the UI is not held out while the card works; the song is changed
    /// only once the clip is written.
    SongOpState doMerge(uint8_t mask, uint32_t fromBar, uint32_t toBar) {
        if (!opened_ || !p.storageReady() || !io_ || !acc_) return SongOpState::Failed;
        AudioSong copy;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            copy = song_;
        }
        /* Muted tracks are not what is heard: left out, left alone. A seq
         * track has no audio to mix until it is bounced: likewise. */
        for (int t = 0; t < kTracks; ++t) {
            if (copy.tracks[t].muted || copy.tracks[t].kind == TrackKind::Seq) mask &= (uint8_t)~(1u << t);
        }
        if (!mask) return SongOpState::Failed;
        for (int t = 0; t < kTracks; ++t) {
            if (!(mask & (1u << t))) continue;
            for (const auto& c : copy.tracks[t].clips) {
                if (c.bar >= fromBar && c.bar < toBar) toBar = std::max<uint32_t>(toBar, c.bar + c.bars);
            }
        }
        const Arrangement arr = flatten(copy);
        const uint32_t spBar = arr.samplesPerBar;
        const uint32_t a = fromBar * spBar, b = toBar * spBar;
        std::string file;
        for (int n = 1; n < 1000 && file.empty(); ++n) {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "merge_%02d.wav", n);
            if (!fsx::exists(copy.clipPath(buf))) file = buf;
        }
        if (file.empty()) return SongOpState::Failed;
        const std::string path = copy.clipPath(file);

        endPlayback();
        WavWriter w;
        if (!w.open(path.c_str(), sampleRate_, 2)) return SongOpState::Failed;
        w.reserve(b - a);                           // the card's clusters found now, not per chunk
        ClipReader rd[kTracks];
        bool ok = true;
        for (uint32_t f = a; f < b && ok;) {
            const uint32_t n = std::min(kChunkFrames, b - f);
            std::memset(acc_, 0, n * 8);
            for (int t = 0; t < kTracks; ++t) {
                if (!(mask & (1u << t))) continue;
                const float g = copy.tracks[t].volume;
                for (const auto& sg : arr.tracks[t]) {
                    if (sg.end <= f) continue;
                    if (sg.start >= f + n) break;
                    const uint32_t x = std::max(sg.start, f), y = std::min(sg.end, f + n);
                    if (rd[t].path != sg.path && !(p.storageReady() && rd[t].open(sg.path))) continue;
                    rd[t].read(x - sg.start + sg.offset, io_, y - x);
                    int32_t* d = acc_ + (x - f) * 2;
                    for (uint32_t k = 0; k < (y - x) * 2; ++k) d[k] += (int32_t)((float)io_[k] * g);
                }
            }
            for (uint32_t k = 0; k < n * 2; ++k) {
                const int32_t v = acc_[k];
                io_[k] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            }
            ok = w.write(io_, n);
            f += n;
            editProgress_.store((uint8_t)((uint64_t)(f - a) * 99 / (b - a)));
            p.yieldIo();
        }
        for (auto& r : rd) r.close();
        if (!w.close() || !ok) {
            std::remove(path.c_str());
            return SongOpState::Failed;
        }

        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            int target = -1;
            for (int t = 0; t < kTracks; ++t) {
                if (!(mask & (1u << t))) continue;
                if (target < 0) target = t;
                auto& cs = song_.tracks[t].clips;
                cs.erase(std::remove_if(cs.begin(), cs.end(), [&](const SongClip& c) {
                    return c.bar >= fromBar && c.bar < toBar;
                }), cs.end());
                /* A block reaching into the range from before it keeps what
                 * is before it; the rest is in the new clip. */
                for (auto& c : cs) {
                    if (c.bar < fromBar && c.bar + c.bars > fromBar) c.bars = (uint16_t)(fromBar - c.bar);
                }
            }
            SongClip c;
            c.file = file;
            c.bar = fromBar;
            c.bars = (uint16_t)std::max<uint32_t>(1, toBar - fromBar);
            song_.addClip(target, c);
            song_.tracks[target].volume = 1.0f;      // the tracks' volumes are in the clip now
            rebuild();
        }
        doSave();
        editProgress_.store(100);
        logf(false, "merged tracks 0x%x, bars %u..%u, into %s", (unsigned)mask, (unsigned)fromBar,
             (unsigned)toBar, file.c_str());
        return SongOpState::Ready;
    }

    /// Worker: a region of a WAV into the project's clips/, laid on a track
    /// -- see ISongEngine::requestImport. The clip is named after its source,
    /// numbered when the project has one of that name already.
    /// Worker: the open song as a Live Set in its own folder -- see
    /// ISongEngine::requestExportLiveSet. The song is copied, the clips'
    /// headers read, and the Set written to a .tmp that only replaces the
    /// .als once it is whole.
    SongOpState doExport() {
        AudioSong copy;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            copy = song_;
        }
        std::vector<std::string> lines;
        auto finish = [&](SongOpState st) {
            for (const auto& ln : lines) logf(st == SongOpState::Failed, "export: %s", ln.c_str());
            std::lock_guard<std::mutex> l(reportMutex_);
            report_ = std::move(lines);
            return st;
        };
        if (!opened_ || copy.dir.empty() || !p.storageReady()) {
            lines.push_back("No song open, or no card.");
            return finish(SongOpState::Failed);
        }
        endPlayback();

        /* Each file once; one not on the card stays off the list, which is
         * how the report tells missing from unreadable. */
        std::vector<LiveSetClipFile> files;
        std::vector<std::string> seen;
        for (const auto& t : copy.tracks) {
            for (const auto& c : t.clips) {
                if (std::find(seen.begin(), seen.end(), c.file) != seen.end()) continue;
                seen.push_back(c.file);
                LiveSetClipFile f;
                f.file = c.file;
                const std::string path = copy.clipPath(c.file);
                if (!fsx::fileInfo(path, f.bytes, f.modified)) continue;
                WavInfo info;
                if (wavReadInfo(path.c_str(), info)) {
                    f.frames = info.frames;
                    f.sampleRate = info.sampleRate;
                }
                files.push_back(f);
                p.yieldIo();
            }
        }

        std::string skeleton = p.liveSetTemplate();
        if (skeleton.empty()) {
            lines.push_back("Not enough memory to build the Set (" +
                            std::to_string(p.freeMemoryBytes() / 1024) + " kB free).");
            return finish(SongOpState::Failed);
        }
        /* An empty Project12.cfg makes the folder a Live project, and that is
         * what the clips' paths are relative to; Live fills it in on its
         * first save. Without it the Set opens with every file missing. */
        const std::string projectInfo = copy.dir + "/Ableton Project Info";
        const std::string cfg = projectInfo + "/Project12.cfg";
        if (!fsx::exists(cfg)) {
            std::FILE* f = fsx::makeDir(projectInfo) ? std::fopen(cfg.c_str(), "wb") : nullptr;
            if (!f) {
                lines.push_back("The card would not take the project's folder.");
                return finish(SongOpState::Failed);
            }
            std::fclose(f);
        }

        const std::string als = copy.dir + "/" + copy.name + ".als";
        const std::string tmp = als + ".tmp";
        GzipStoredWriter gz;
        LiveSetReport rep;
        bool ok = gz.open(tmp) &&
                  writeLiveSetXml(std::move(skeleton), copy, files,
                                  [&](const char* d, size_t n) { p.yieldIo(); return gz.write(d, n); }, rep);
        ok = gz.close() && ok;
        if (!ok) {
            std::remove(tmp.c_str());
            lines.push_back("The Set was not written: the card is full or would not take it.");
            return finish(SongOpState::Failed);
        }
        /* One Live saved over is not lost: FatFs will not rename onto a file,
         * and the one there goes to Backup/ rather than away. */
        std::string kept;
        if (fsx::exists(als)) {
            const std::string backup = copy.dir + "/Backup";
            for (int n = 1; n < 1000 && kept.empty(); ++n) {
                const std::string name = copy.name + " [" + std::to_string(n) + "].als";
                if (!fsx::exists(backup + "/" + name)) kept = name;
            }
            if (kept.empty() || !fsx::makeDir(backup) || !fsx::rename(als, backup + "/" + kept)) {
                std::remove(tmp.c_str());
                lines.push_back("The Set already there could not be moved to Backup/; nothing was changed.");
                return finish(SongOpState::Failed);
            }
        }
        if (!fsx::rename(tmp, als)) {
            lines.push_back("The Set was written but could not be named " + copy.name + ".als.");
            return finish(SongOpState::Failed);
        }
        std::string root = p.songsRoot();
        root = root.substr(root.find_last_of('/') + 1);
        lines.push_back(root + "/" + copy.name + "/" + copy.name + ".als, " + std::to_string(rep.clips) +
                        (rep.clips == 1 ? " clip" : " clips"));
        if (!kept.empty()) lines.push_back("The one there before is in Backup/ as " + kept);
        lines.insert(lines.end(), rep.notes.begin(), rep.notes.end());
        return finish(SongOpState::Ready);
    }

    SongOpState doImport() {
        std::string src;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            src = editName_;
        }
        if (!opened_ || !io_ || !p.storageReady()) return SongOpState::Failed;
        ClipReader r;
        if (!r.open(src)) return SongOpState::Failed;
        uint32_t a = importA_, b = importB_;
        if (!b || b > r.frames) b = r.frames;
        if (a >= b) { r.close(); return SongOpState::Failed; }
        std::string stem = src.substr(src.find_last_of("/\\") + 1);
        if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".wav") == 0) stem.resize(stem.size() - 4);
        if (stem.size() > kMaxClipPrefix) stem.resize(kMaxClipPrefix);
        std::string file, path;
        for (int n = 1; n < 100 && file.empty(); ++n) {
            const std::string cand = n == 1 ? stem + ".wav" : stem + "_" + std::to_string(n) + ".wav";
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            if (!fsx::exists(song_.clipPath(cand))) { file = cand; path = song_.clipPath(cand); }
        }
        if (file.empty()) { r.close(); return SongOpState::Failed; }

        endPlayback();                              // the worker cannot keep the ring filled meanwhile
        const uint32_t lead = importLead_, total = lead + (b - a);
        WavWriter w;
        if (!w.open(path.c_str(), sampleRate_, 2)) { r.close(); return SongOpState::Failed; }
        w.reserve(total);
        bool ok = true;
        uint32_t done = 0;
        std::memset(io_, 0, kChunkFrames * 4);
        while (ok && done < lead) {
            const uint32_t n = std::min(kChunkFrames, lead - done);
            ok = w.write(io_, n);
            done += n;
        }
        for (uint32_t f = a; ok && f < b;) {
            const uint32_t n = std::min(kChunkFrames, b - f);
            r.read(f, io_, n);
            ok = w.write(io_, n);
            f += n;
            done += n;
            editProgress_.store((uint8_t)((uint64_t)done * 99 / total));
            p.yieldIo();
        }
        r.close();
        if (!w.close() || !ok) {
            std::remove(path.c_str());
            return SongOpState::Failed;
        }

        const uint32_t bar = importBar_;
        uint8_t t = importTrack_;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            const int target = audioTarget(importTrack_, true);
            if (target < 0) {
                std::remove(path.c_str());
                return SongOpState::Failed;
            }
            t = (uint8_t)target;
            const uint32_t spBar = song_.samplesPerBar(sampleRate_);
            const uint32_t bars = std::max<uint32_t>(1, std::min<uint32_t>((total + spBar - 1) / spBar, 999));
            auto& cs = song_.tracks[t].clips;
            cs.erase(std::remove_if(cs.begin(), cs.end(), [&](const SongClip& c) {
                return c.bar >= bar && c.bar < bar + bars;
            }), cs.end());
            for (auto& c : cs) {
                if (c.bar < bar && c.bar + c.bars > bar) c.bars = (uint16_t)(bar - c.bar);
            }
            SongClip c;
            c.file = file;
            c.bar = bar;
            c.bars = (uint16_t)bars;
            song_.addClip(t, c);
            rebuild();
        }
        doSave();
        editProgress_.store(100);
        logf(false, "imported %s [%u, %u) +%u to track %u bar %u as %s", src.c_str(), (unsigned)a,
             (unsigned)b, (unsigned)lead, (unsigned)(t + 1), (unsigned)bar, file.c_str());
        return SongOpState::Ready;
    }

    /// Worker: write the song. A copy is taken under the lock so the card
    /// write does not hold the UI out.
    void doSave() {
        AudioSong copy;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            if (!opened_) return;
            copy = song_;
        }
        const int64_t t0 = p.nowUs();
        if (p.storageReady() && !copy.save()) logf(true, "song.json not written");
        dbgMax(dbgSaveMaxUs_, p.nowUs() - t0);
    }

    /// @p song as sample ranges. A later block cuts the one it lands on, as
    /// on any timeline, and the song's length -- or the loop region, where it
    /// reaches further -- cuts everything past it.
    /// The seq track that plays with the song, or -1: in use, a seq, and
    /// not muted -- muted, the sequencer is simply not started with it.
    static int seqTrackOf(const AudioSong& song, bool evenMuted = false) {
        for (int t = 0; t < kTracks; ++t) {
            const SongTrack& tr = song.tracks[t];
            if (tr.inUse && tr.kind == TrackKind::Seq && (evenMuted || !song.silenced(t))) return t;
        }
        return -1;
    }

    /// AudioSong::playEnd(), and with no length set at least one pass of the
    /// seq track's scene: that is how long a song of only a seq track is.
    uint32_t endBars(const AudioSong& song) const {
        uint32_t end = song.playEnd();
        if (song.length) return end;
        const int t = seqTrackOf(song, true);
        /* Only a track that plays its scene over everything: one cut into
         * blocks is as long as its blocks, like an audio track. */
        if (t >= 0 && !song.tracks[t].seqBlocks) {
            const uint8_t bars = seq_.sceneBars(song.tracks[t].scene);
            end = std::max<uint32_t>(end, bars ? bars : 1);
        }
        return end;
    }

    Arrangement flatten(const AudioSong& song) const {
        Arrangement a;
        a.samplesPerBar = song.samplesPerBar(sampleRate_);
        a.loop = song.loop;
        a.end = endBars(song) * a.samplesPerBar;
        /* A take running past the song's last block runs the song on with
         * it: at the old end it would wrap to bar one, or stop. */
        if (takeHorizon_) a.end = std::max<uint32_t>(a.end, kTakeHorizonBars * a.samplesPerBar);
        if (song.hasRegion()) {
            a.loopA = song.loopStart * a.samplesPerBar;
            a.loopB = song.loopEnd * a.samplesPerBar;
        }
        const uint32_t cut = std::max(a.end, a.loopB);
        const int seqTrack = seqTrackOf(song, true);
        if (seqTrack >= 0 && song.tracks[seqTrack].seqBlocks) {
            /* On the song's grid: the song sets the clock's tempo when it
             * starts the sequencer with it. */
            a.seqSpans = seqSpansOf(song.tracks[seqTrack], a.samplesPerBar, song.seqTicksPerBar(), cut);
            a.seqMapped = true;
        }
        for (int t = 0; t < kTracks; ++t) {
            if (song.tracks[t].kind == TrackKind::Seq) continue;   // no audio of its own
            const auto& cs = song.tracks[t].clips;
            for (size_t i = 0; i < cs.size(); ++i) {
                Segment s;
                s.start = cs[i].bar * a.samplesPerBar;
                s.end   = (cs[i].bar + cs[i].bars) * a.samplesPerBar;
                if (i + 1 < cs.size()) s.end = std::min(s.end, cs[i + 1].bar * a.samplesPerBar);
                s.end = std::min(s.end, cut);
                /* A stretched track plays its block's fitted copy, which fills
                 * the block's bars from its first frame -- once song_io has
                 * made it. Known from memory, not the card: this runs under
                 * the song's lock, on whatever thread committed. */
                const StretchMode mode = song.tracks[t].stretch;
                const std::string fitted = mode != StretchMode::Off
                    ? song.clipPath(stretchName(cs[i], cs[i].bars * a.samplesPerBar, mode)) : std::string();
                if (!fitted.empty() && stretchReady(fitted)) {
                    s.path = fitted;
                } else {
                    s.path = song.clipPath(cs[i].file);
                    if (cs[i].frames) s.end = std::min(s.end, s.start + cs[i].frames);
                    s.offset = cs[i].offset;
                }
                if (s.end > s.start) a.tracks[t].push_back(s);
            }
        }
        return a;
    }

    /// A seq track's blocks as stretches of the song at @p spb samples a
    /// bar (@p tpb ticks): each from its bar to its end or the next block's,
    /// up to @p cut, with its own notes as samples -- or, a block that plays
    /// the scene, the scene sample its bar of the scene starts on.
    std::vector<PatternSequencer::SongSpan> seqSpansOf(const SongTrack& tr, uint32_t spb, uint32_t tpb,
                                                       uint32_t cut) const {
        std::vector<PatternSequencer::SongSpan> out;
        const uint8_t sb = seq_.sceneBars(tr.scene);
        const auto& cs = tr.clips;
        for (size_t i = 0; i < cs.size(); ++i) {
            uint32_t b = (cs[i].bar + cs[i].bars) * spb;
            if (i + 1 < cs.size()) b = std::min(b, cs[i + 1].bar * spb);
            if (cut) b = std::min(b, cut);
            const uint32_t a = cs[i].bar * spb;
            if (b <= a) continue;
            PatternSequencer::SongSpan s;
            s.a = a;
            s.b = b;
            if (cs[i].notes) {
                const uint32_t bars = cs[i].notes->bars ? cs[i].notes->bars : 1;
                s.notes = songNotes(cs[i].notes, spb, tpb);
                s.loop = bars * spb;
                s.scene = (uint32_t)(cs[i].from % bars) * spb;
            } else {
                s.scene = (uint32_t)(cs[i].from % (sb ? sb : 1)) * spb;
            }
            out.push_back(std::move(s));
        }
        return out;
    }

    /// @p p as the sequencer plays it, at @p spb samples (@p tpb ticks) a
    /// bar. Made once per pattern and tempo: a block's copies share it, and
    /// a commit that left the notes alone -- a fader -- converts nothing.
    PatternSequencer::SongNotes songNotes(const SeqPatternPtr& p, uint32_t spb, uint32_t tpb) const {
        std::lock_guard<std::mutex> l(noteCacheMutex_);
        for (const auto& e : noteCache_)
            if (e.pattern == p && e.spb == spb && e.tpb == tpb) return e.notes;
        auto v = std::make_shared<std::vector<PatternSequencer::SongNote>>();
        v->reserve(p->notes.size());
        const uint32_t t = tpb ? tpb : 1;
        for (const SeqNote& n : p->notes) {
            PatternSequencer::SongNote o;
            o.pos = (uint32_t)((uint64_t)n.tick * spb / t);
            o.len = (uint32_t)((uint64_t)n.len * spb / t);
            o.pad = n.pad;
            o.vel = n.vel;
            o.perf = n.perf;
            v->push_back(o);
        }
        if (noteCache_.size() >= kNoteCacheSize) noteCache_.erase(noteCache_.begin());
        noteCache_.push_back({p, spb, tpb, v});
        return v;
    }

    /// The sequencer laid on what the song plays now: the seq track's blocks,
    /// wrapping where the song does -- or, with none, its own loop. Worker.
    void pushSeqMap() {
        std::vector<PatternSequencer::SongSpan> spans;
        bool mapped = false;
        uint32_t wrapFrom = 0, wrapTo = 0;
        {
            std::lock_guard<std::mutex> l(snapMutex_);
            mapped = snap_.seqMapped;
            if (mapped) spans = snap_.seqSpans;
            const bool region = snap_.loopB > snap_.loopA;
            wrapFrom = snap_.loop ? (region ? snap_.loopB : snap_.end) : 0;
            wrapTo = region ? snap_.loopA : 0;
            mapSentGen_ = snapGen_.load();
        }
        if (mapped) seq_.setSongMap(spans, wrapFrom, wrapTo);
        else        seq_.clearSongMap();
    }

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

    /// The arrangement as bytes -- tempo, metre, loop, length and the four
    /// tracks with their blocks; not the project's name or folder, and not
    /// the launcher's sets. One allocation, so a step costs PSRAM rather than
    /// internal RAM for every block's name; and equal arrangements are equal
    /// bytes, which is how a change is told from a commit that changed nothing.
    static void pack(const AudioSong& s, Snapshot& out) {
        out.bytes.clear();
        out.notes.clear();
        out.bytes.reserve(512);
        auto put = [&](const void* v, size_t n) {
            const uint8_t* b = static_cast<const uint8_t*>(v);
            out.bytes.insert(out.bytes.end(), b, b + n);
        };
        auto str = [&](const std::string& v) {
            const uint16_t n = (uint16_t)std::min<size_t>(v.size(), 0xFFFF);
            put(&n, 2);
            put(v.data(), n);
        };
        const uint8_t flags = (s.loop ? 1 : 0) | (s.syncSeq ? 2 : 0);
        put(&s.bpm, sizeof s.bpm);
        put(&s.beatsPerBar, 1);
        put(&flags, 1);
        put(&s.length, 4);
        put(&s.loopStart, 4);
        put(&s.loopEnd, 4);
        for (const auto& t : s.tracks) {
            const uint8_t head[5] = {
                (uint8_t)((t.inUse ? 1 : 0) | (t.muted ? 2 : 0) | (t.solo ? 4 : 0) | (t.seqBlocks ? 8 : 0)),
                (uint8_t)t.kind, (uint8_t)t.source, (uint8_t)t.stretch, t.scene};
            put(head, sizeof head);
            put(&t.volume, sizeof t.volume);
            str(t.name);
            const uint16_t n = (uint16_t)t.clips.size();
            put(&n, 2);
            for (const auto& c : t.clips) {
                str(c.file);
                put(&c.bar, 4);
                put(&c.bars, 2);
                put(&c.offset, 4);
                put(&c.frames, 4);
                put(&c.from, 2);
                uint16_t ni = 0xFFFF;
                if (c.notes) {
                    const auto it = std::find(out.notes.begin(), out.notes.end(), c.notes);
                    ni = (uint16_t)(it - out.notes.begin());
                    if (it == out.notes.end()) out.notes.push_back(c.notes);
                }
                put(&ni, 2);
            }
        }
    }

    /// pack() undone, into @p s: its name, folder and sets are left alone.
    static void unpack(const Snapshot& in, AudioSong& s) {
        size_t at = 0;
        auto get = [&](void* v, size_t n) { std::memcpy(v, in.bytes.data() + at, n); at += n; };
        auto str = [&](std::string& v) {
            uint16_t n = 0;
            get(&n, 2);
            v.assign(reinterpret_cast<const char*>(in.bytes.data()) + at, n);
            at += n;
        };
        uint8_t flags = 0;
        get(&s.bpm, sizeof s.bpm);
        get(&s.beatsPerBar, 1);
        get(&flags, 1);
        s.loop = flags & 1;
        s.syncSeq = flags & 2;
        get(&s.length, 4);
        get(&s.loopStart, 4);
        get(&s.loopEnd, 4);
        for (auto& t : s.tracks) {
            uint8_t head[5] = {};
            get(head, sizeof head);
            t.inUse = head[0] & 1;
            t.muted = head[0] & 2;
            t.solo = head[0] & 4;
            t.seqBlocks = head[0] & 8;
            t.kind = (TrackKind)head[1];
            t.source = (SeqSource)head[2];
            t.stretch = (StretchMode)head[3];
            t.scene = head[4];
            get(&t.volume, sizeof t.volume);
            str(t.name);
            uint16_t n = 0;
            get(&n, 2);
            t.clips.assign(n, SongClip{});
            for (auto& c : t.clips) {
                str(c.file);
                get(&c.bar, 4);
                get(&c.bars, 2);
                get(&c.offset, 4);
                get(&c.frames, 4);
                get(&c.from, 2);
                uint16_t ni = 0xFFFF;
                get(&ni, 2);
                c.notes = ni < in.notes.size() ? in.notes[ni] : nullptr;
            }
        }
    }

    /// rebuild()'s first step, under songMutex_: if the arrangement moved
    /// since it was last seen, what it was becomes an undo step -- one per
    /// burst, so a fader pulled or a block nudged detent by detent goes back
    /// at once -- and what could be redone is gone.
    void noteChange() {
        Snapshot now;
        pack(song_, now);
        if (now == last_) return;
        const int64_t t = p.nowUs();
        if (!last_.empty() && (undo_.empty() || t - lastChangeUs_ > kUndoMergeUs)) {
            undo_.push_back(std::move(last_));
            if (undo_.size() > kUndoDepth) undo_.erase(undo_.begin());
        }
        redo_.clear();
        last_ = std::move(now);
        lastChangeUs_ = t;
        undoCount_.store((uint32_t)undo_.size());
        redoCount_.store(0);
    }

    /// A job that took a copy of the song to write back: a step now would
    /// be written over when it lands.
    bool songJobRunning() {
        return editReq_.load() != 0 || editState_.load() == SongOpState::Busy || openReq_.load() ||
               bounceReq_.load() || bounceStatus().state == BounceState::Running ||
               seqTake_.load() != SyncTake::None;
    }

    /// Back (or forward again) one step: what is there now goes on the other
    /// stack. Only the arrangement moves -- the audio on the card is never
    /// touched, so a block undone back in finds its file where it was.
    bool step(std::vector<Snapshot>& from, std::vector<Snapshot>& to) {
        if (songJobRunning()) return false;
        float was = 0.0f, now = 0.0f;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            if (!opened_ || from.empty()) return false;
            Snapshot cur;
            pack(song_, cur);
            to.push_back(std::move(cur));
            if (to.size() > kUndoDepth) to.erase(to.begin());
            was = song_.bpm;
            unpack(from.back(), song_);
            last_ = std::move(from.back());
            from.pop_back();
            lastChangeUs_ = 0;           // the next edit is a step of its own
            now = song_.bpm;
            undoCount_.store((uint32_t)undo_.size());
            redoCount_.store((uint32_t)redo_.size());
            rebuild();
        }
        /* A tempo undone is a tempo change: the song stops, as doTempo
         * stops it, and the clock follows. */
        if (std::fabs(now - was) > 0.01f) {
            stop();
            if (now > 0.0f) seq_.setClickBpm(now);
        }
        saveAtUs_.store(p.nowUs() + (int64_t)kSaveDelayMs * 1000);
        saveReq_.store(true);
        p.wake();
        return true;
    }
    /// Worker: track @p t's loudest frames @p m (left, right) in hop @p hop
    /// of the ring.
    void meterPut(int t, uint32_t hop, const int32_t m[2]) {
        Meter& s = meter_[hop % kMeterSlots];
        if (s.hop.load(std::memory_order_relaxed) != hop) {
            for (auto& tr : s.peak) for (auto& p : tr) p.store(0, std::memory_order_relaxed);
            s.hop.store(hop, std::memory_order_release);
        }
        for (int c = 0; c < 2; ++c) {
            const uint16_t v = (uint16_t)std::min<int32_t>(m[c], 32767);
            if (v > s.peak[t][c].load(std::memory_order_relaxed)) s.peak[t][c].store(v, std::memory_order_relaxed);
        }
    }
    /// A ring that starts again from frame 0 has no meters yet.
    void meterReset() {
        for (auto& s : meter_) s.hop.store(UINT32_MAX, std::memory_order_relaxed);
    }

    /// The song for the worker, and the per-track gains and loop for RT --
    /// so a mute or a fader acts at once while playing. Caller holds
    /// songMutex_.
    void rebuild() {
        noteChange();
        Arrangement a = flatten(song_);
        for (int t = 0; t < kTracks; ++t) {
            const auto& tr = song_.tracks[t];
            gain_[t].store(song_.silenced(t) ? 0.0f : tr.volume);
        }
        seqTrack_.store((int8_t)seqTrackOf(song_, true), std::memory_order_relaxed);
        /* Counted here too, not only by song_io: it asks only while
         * stopped, and a tempo turned while playing must show at once. */
        nextStretch(nullptr);
        /* The live transport has a grid of its own and no end; a commit
         * made while it runs (a fader, a mute) must not hand RT the
         * arrangement's. */
        const bool live = liveOn_.load();
        if (!live) {
            loop_.store(a.loop);
            spBar_.store(a.samplesPerBar);
        }
        {
            std::lock_guard<std::mutex> l(snapMutex_);
            if (!live) {
                end_.store(a.end);
                loopA_.store(a.loopA);
                loopB_.store(a.loopB);
            }
            snap_ = std::move(a);
            snapGen_.fetch_add(1);
        }
    }

    /// Ready the song's seq track for a take: made if there is none (the
    /// kit, on the lowest free track), its blocks given the scene's notes as
    /// their own, and the sequencer keeping hits. A REC is not heard under
    /// itself. Caller holds songMutex_.
    bool openTake(SyncTake take) {
        int t = seqTrackOf(song_, true);
        const bool made = t < 0;
        if (made) {
            t = song_.freeTrack();
            /* A scene of its own, which the seq view edits the track's
             * blocks in -- not the groove up in the Sequencer. With no room
             * for another the one up is lent, as Arrange's SEQ card does. */
            if (t < 0) return false;
            seq_.addScene();
            if (!song_.addSeqTrack(t, SeqSource::Kit, seq_.activeScene())) return false;
            /* A song that starts as a seq track has no clip to take its
             * tempo from: its grid is the sequencer's. */
            if (song_.bpm <= 0.0f) song_.bpm = seq_.getClickBpm();
        }
        if (!seq_.beginSongTake(kTakeRoom)) return false;
        /* A track made for the take holds the take and nothing else; one
         * that played its scene keeps playing what it played. */
        if (!made) song_.adoptSceneNotes(t, scenePattern(song_.tracks[t].scene), endBars(song_));
        song_.tracks[t].seqBlocks = true;
        takeTrack_ = t;
        takeCycle_ = song_.loop && song_.hasRegion();
        takeReplaced_ = false;
        takeLaps_ = 0;
        takeNotes_.clear();
        takeHorizon_ = !takeCycle_;
        seq_.setSongMapMuted(take == SyncTake::Record);
        seqTake_.store(take);
        commit();
        return true;
    }

    /// The block of @p tr sounding on @p bar, or -1.
    static int blockAt(const SongTrack& tr, uint32_t bar) {
        const auto& cs = tr.clips;
        for (size_t i = 0; i < cs.size(); ++i) {
            uint32_t end = cs[i].bar + cs[i].bars;
            if (i + 1 < cs.size()) end = std::min(end, cs[i + 1].bar);
            if (bar >= cs[i].bar && bar < end) return (int)i;
        }
        return -1;
    }

    /// Bars free on @p tr from @p bar up to its next block, at least one.
    static uint32_t roomAt(const SongTrack& tr, uint32_t bar) {
        uint32_t room = 0xFFFF;
        for (const auto& c : tr.clips) if (c.bar > bar) room = std::min(room, c.bar - bar);
        return std::max<uint32_t>(room, 1);
    }

    static bool samePattern(const SeqPatternPtr& a, const SeqPatternPtr& b) {
        if (a == b) return true;
        if (!a || !b || a->bars != b->bars || a->notes.size() != b->notes.size()) return false;
        for (size_t i = 0; i < a->notes.size(); ++i) {
            const SeqNote& x = a->notes[i];
            const SeqNote& y = b->notes[i];
            if (x.tick != y.tick || x.len != y.len || x.pad != y.pad || x.vel != y.vel || x.perf != y.perf) return false;
        }
        return true;
    }

    /// The take is off: the sequencer keeps no more hits, the track is heard
    /// again and the song ends where its blocks do. Caller holds songMutex_.
    void closeTake() {
        seq_.endSongTake();
        seq_.setSongMapMuted(false);
        seqTake_.store(SyncTake::None);
        takeTrack_ = -1;
        takeNotes_.clear();
        if (takeHorizon_) {
            takeHorizon_ = false;
            rebuild();
        }
    }

    /// Scene @p scene's notes as a pattern of its own: what a block that
    /// played the scene is given before a take changes it.
    SeqPatternPtr scenePattern(uint8_t scene) const {
        std::vector<SeqNote> notes;
        const uint16_t n = seq_.eventCount(scene);
        notes.reserve(n);
        for (uint16_t i = 0; i < n; ++i) {
            uint32_t ticks = 0;
            uint8_t pad = 0, vel = 0;
            if (!seq_.eventAtTicks(scene, i, ticks, pad, vel)) continue;
            SeqNote o;
            o.tick = ticks;
            o.len = seq_.eventLengthTicks(scene, i);
            o.pad = pad;
            o.vel = vel;
            o.perf = seq_.eventPerformance(scene, i).pack();
            notes.push_back(o);
        }
        const uint8_t bars = seq_.sceneBars(scene);
        return makeSeqPattern(bars ? bars : 1, std::move(notes));
    }

    /// Worker, while it streams: the hits taken so far are moved out of the
    /// sequencer, and a lap of the region that is over lands.
    void serviceTake() {
        if (seqTake_.load() == SyncTake::None) return;
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (seqTake_.load() == SyncTake::None) return;
        seq_.takeSongNotes(takeNotes_, false);
        const uint16_t laps = seq_.songTakeLaps();
        if (takeCycle_ && laps != takeLaps_) landLaps(laps);
    }

    /// Every lap before @p upto lands, one pass each: a REC's first pass with
    /// notes replaces the region, every pass after it is dubbed in -- and
    /// once the first has landed the track is heard, so the next lap plays
    /// along with what was just played. Caller holds songMutex_.
    void landLaps(uint16_t upto) {
        const uint32_t spb = song_.samplesPerBar(sampleRate_);
        for (; takeLaps_ != upto; ++takeLaps_) {
            std::vector<PatternSequencer::SongTakeNote> pass, rest;
            for (const auto& n : takeNotes_) (n.lap == takeLaps_ ? pass : rest).push_back(n);
            takeNotes_.swap(rest);
            const bool replace = seqTake_.load() == SyncTake::Record && !takeReplaced_;
            const uint32_t from = takeLaps_ == 0 ? takeFrom_ : song_.loopStart * spb;
            if (landTake(pass, replace, from, song_.loopEnd * spb) && replace) {
                takeReplaced_ = true;
                seq_.setSongMapMuted(false);
            }
        }
    }

    /// A stretch of the take onto the track: REC as one block from the bar
    /// @p from is in to the bar @p to ends in (and at least past its last
    /// note), the seq blocks under it gone; DUB into the blocks it fell
    /// under. Song samples. Nothing played, nothing changes. Caller holds
    /// songMutex_.
    bool landTake(const std::vector<PatternSequencer::SongTakeNote>& notes, bool replace,
                  uint32_t from, uint32_t to) {
        if (notes.empty() || takeTrack_ < 0) return false;
        const uint32_t spb = song_.samplesPerBar(sampleRate_);
        const uint32_t bpb = song_.beatsPerBar ? song_.beatsPerBar : AudioSong::kDefaultBeatsPerBar;
        const uint32_t beat = spb / bpb ? spb / bpb : 1;
        const uint32_t tpb = song_.seqTicksPerBar();
        auto ticks = [&](uint32_t frames) {
            return (uint32_t)(((uint64_t)frames * AudioSong::kSeqPpq + beat / 2) / beat);
        };
        if (!replace) {
            std::vector<SeqNote> dub;
            dub.reserve(notes.size());
            for (const auto& n : notes) dub.push_back({ticks(n.pos), ticks(n.len), n.pad, n.vel, n.perf});
            song_.dubSeqNotes(takeTrack_, dub);
        } else {
            const uint32_t fromBar = from / spb;
            uint32_t toBar = std::max<uint32_t>(fromBar + 1, (to + spb - 1) / spb);
            for (const auto& n : notes) toBar = std::max<uint32_t>(toBar, n.pos / spb + 1);
            std::vector<SeqNote> mine;
            mine.reserve(notes.size());
            for (const auto& n : notes) {
                const uint32_t t = ticks(n.pos);
                if (t < fromBar * tpb) continue;
                mine.push_back({t - fromBar * tpb, ticks(n.len), n.pad, n.vel, n.perf});
            }
            const uint16_t bars = (uint16_t)std::min<uint32_t>(toBar - fromBar, 0xFFFF);
            song_.putSeqTake(takeTrack_, fromBar, makeSeqPattern(bars, std::move(mine)), spb);
        }
        commit();
        return true;
    }

    /// The take is over -- stopped, or punched out with the song playing on:
    /// what is left of it lands, as far as the song got. A stop a beat or
    /// less past a bar line ends the block on that line, unless a note was
    /// played after it: nobody presses STOP on the downbeat. Caller holds
    /// songMutex_.
    void finishTake() {
        const SyncTake take = seqTake_.load();
        if (take == SyncTake::None) return;
        seq_.takeSongNotes(takeNotes_, true);
        const uint32_t at = seq_.songMapPosition();
        const uint32_t spb = song_.samplesPerBar(sampleRate_);
        const uint32_t bpb = song_.beatsPerBar ? song_.beatsPerBar : AudioSong::kDefaultBeatsPerBar;
        const uint32_t grace = spb / bpb;
        if (takeCycle_) {
            const uint16_t laps = seq_.songTakeLaps();
            if (laps != takeLaps_) landLaps(laps);
        }
        const uint32_t from = takeCycle_ && takeLaps_ ? song_.loopStart * spb : takeFrom_;
        const uint32_t to = at > from + grace ? at - grace : from;
        landTake(takeNotes_, take == SyncTake::Record && !takeReplaced_, from, to);
        closeTake();
    }

    void setStatus(BounceState st, const std::string& err = std::string()) {
        std::lock_guard<std::mutex> l(statusMutex_);
        status_.state = st;
        if (!err.empty()) status_.error = err;
    }

    void worker() {
        while (!quit_.load()) {
            const bool streaming = job_.load() == Job::Play && play_.load() != Play::Idle;
            /* Waveforms are worked out only while nothing plays, one after
             * another without waiting. */
            const bool peaking = !streaming && peaksPending() && p.storageReady();
            /* Stretched copies likewise, after the waveforms. */
            const bool stretching = !streaming && !peaking && stretchPending() && p.storageReady();
            /* Until a project is open, look for the card once a second; with a
             * save waiting, back in time to make it. */
            p.waitWake(streaming ? kStreamPollMs
                                 : peaking || stretching ? 0
                                 : saveReq_.load() ? kSaveDelayMs : (lastTried_ ? UINT32_MAX : kCardPollMs));
            if (quit_.load()) break;

            if (releaseReq_.load()) {
                /* A bounce still queued would allocate into the arena being
                 * wiped; it is refused, the one running was cancelled. */
                if (bounceReq_.exchange(false)) {
                    setStatus(BounceState::Failed, "cancelled: the kit's engine changed");
                    busy_.store(false);
                }
                stopReq_.store(false);
                endPlayback();
                releaseReq_.store(false);
                continue;
            }
            if (stopReq_.exchange(false) || play_.load() == Play::Ended) {
                endPlayback();
                continue;
            }
            if (liveStartReq_.exchange(false)) {
                startLive();
                continue;
            }
            if (playReq_.exchange(false)) {
                startPlayback(playFrom_.load());
                /* A take armed for this playback is its; one that never
                 * started (no memory for the ring) is called off. */
                if (seqTake_.load() != SyncTake::None) {
                    takeBound_ = play_.load() != Play::Idle;
                    if (!takeBound_) {
                        std::lock_guard<std::recursive_mutex> l(songMutex_);
                        closeTake();
                    }
                }
                continue;
            }
            if (openReq_.exchange(false)) {
                std::string name;
                {
                    std::lock_guard<std::recursive_mutex> l(songMutex_);
                    name = openName_;
                }
                /* A project asked for by name is the one open: opening the
                 * last one afterwards would read it back over unsaved edits. */
                lastTried_ = true;
                openState_.store(doOpen(name));
                continue;
            }
            if (const uint8_t edit = editReq_.exchange(0)) {
                editState_.store(edit == kEditRename ? doRename()
                               : edit == kEditDelete ? doDelete()
                               : edit == kEditTempo  ? doTempo(editBpm_.load())
                               : edit == kEditImport ? doImport()
                               : edit == kEditExport ? doExport()
                               : doMerge(editMask_.load(), editFrom_.load(), editTo_.load()));
                continue;
            }
            if (const uint8_t lists = listReq_.exchange(0)) {
                if (lists & kListProjects) doListProjects();
                if (lists & kListClips)    doListClips();
                listGen_.fetch_add(1);
                continue;
            }
            if (!lastTried_) openLast();
            /* Not while it streams: a song.json written on this card takes the
             * worker off the ring for longer than the ring lasts. The save
             * waits for the stop, which is never far off on a loop. */
            if (!streaming && saveReq_.load() && p.nowUs() >= saveAtUs_.load() && saveReq_.exchange(false)) doSave();
            if (bounceReq_.exchange(false)) {
                runBounce();
                busy_.store(false);
                continue;
            }
            if (job_.load() == Job::Play && play_.load() != Play::Idle) {
                serviceTake();
                fillRings();
            }
            else if (peaksPending() && p.storageReady()) doPeaks();
            else if (stretchPending() && p.storageReady()) doStretch();
        }
    }

    // ── Stretch ───────────────────────────────────────────────────────────
    /// Where block @p c fitted to @p outFrames lives: clips/.stretch/, named
    /// for its file, the part of it the block plays, the length it is made
    /// to and how (an "r" for repitched) -- a tempo, a cut or a mode that
    /// changes any of them is another file, so a stale copy is never played.
    static std::string stretchName(const SongClip& c, uint32_t outFrames, StretchMode mode) {
        std::string stem = c.file;
        const size_t dot = stem.rfind('.');
        if (dot != std::string::npos) stem.resize(dot);
        char tail[48];
        std::snprintf(tail, sizeof(tail), "_%u_%u_%u%s.wav", (unsigned)c.offset, (unsigned)c.frames,
                      (unsigned)outFrames, mode == StretchMode::Repitch ? "r" : "");
        return std::string(kStretchDir) + "/" + stem + tail;
    }

    bool stretchReady(const std::string& path) const {
        std::lock_guard<std::mutex> l(stretchMutex_);
        return stretchReady_.count(path) != 0;
    }

    struct StretchJob { std::string src, dst; uint32_t offset = 0, frames = 0, outFrames = 0; bool repitch = false; };

    /// The blocks of stretched tracks still without a copy, and the first of
    /// them in @p job. Counted for stretchWaiting().
    uint32_t nextStretch(StretchJob* job) {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (!opened_) { stretchCount_.store(0); return 0; }
        const uint32_t spb = song_.samplesPerBar(sampleRate_);
        uint32_t pending = 0;
        std::lock_guard<std::mutex> s(stretchMutex_);
        for (const auto& tr : song_.tracks) {
            if (!tr.inUse || tr.stretch == StretchMode::Off || tr.kind != TrackKind::Audio) continue;
            for (const auto& c : tr.clips) {
                if (c.file.empty()) continue;
                const std::string dst = song_.clipPath(stretchName(c, c.bars * spb, tr.stretch));
                if (stretchReady_.count(dst) || stretchSkip_.count(dst)) continue;
                if (!pending++ && job) {
                    job->src = song_.clipPath(c.file);
                    job->dst = dst;
                    job->offset = c.offset;
                    job->frames = c.frames;
                    job->outFrames = c.bars * spb;
                    job->repitch = tr.stretch == StretchMode::Repitch;
                }
            }
        }
        stretchCount_.store(pending);
        return pending;
    }
    bool stretchPending() { return nextStretch(nullptr) != 0; }

    /// Worker: the first block still without its copy -- read, stretched or
    /// repitched (TimeStretch) and written beside the clips, renamed in when
    /// whole.
    /// Only while nothing plays; a request coming in stops it, and it starts
    /// again from the beginning after.
    void doStretch() {
        StretchJob j;
        if (!nextStretch(&j)) return;
        auto skip = [&](const char* why) {
            std::lock_guard<std::mutex> l(stretchMutex_);
            stretchSkip_.insert(j.dst);
            if (why) logf(true, "stretch %s: %s", j.dst.c_str(), why);
        };
        auto ready = [&]() {
            {
                std::lock_guard<std::mutex> l(stretchMutex_);
                stretchReady_.insert(j.dst);
            }
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            rebuild();
        };
        if (fsx::exists(j.dst)) { ready(); return; }
        ClipReader r;
        if (!r.open(j.src)) { skip("cannot read the clip"); return; }
        const uint32_t rest = r.frames > j.offset ? r.frames - j.offset : 0;
        const uint32_t in = j.frames ? std::min(j.frames, rest) : rest;
        if (!in) { skip("nothing to stretch"); return; }
        const double ratio = (double)j.outFrames / in;
        if (std::fabs(ratio - 1.0) < kStretchSame) { skip(nullptr); return; }
        if (ratio < TimeStretch::kMinRatio || ratio > TimeStretch::kMaxRatio) {
            skip("too far from the song's tempo");
            return;
        }
        /* The ring segments' allocator: the engine arena first, and never the
         * last of the PSRAM the UI lives in. Given back below, and before any
         * arena reset -- releaseRings() is a request, which stops the run. */
        void* work = p.allocRingSegment(TimeStretch::workBytes(), 1);
        if (!work) { skip("not enough memory"); return; }
        const size_t slash = j.dst.find_last_of('/');
        fsx::makeDir(j.dst.substr(0, slash));
        const std::string tmp = j.dst + ".tmp";
        WavWriter w;
        if (!w.open(tmp.c_str(), sampleRate_, 2)) {
            p.freeRingSegment(work);
            skip("card write failed");
            return;
        }
        struct Ctx { Impl* self; ClipReader* r; WavWriter* w; uint32_t offset; uint32_t hops; };
        Ctx ctx{this, &r, &w, j.offset, 0};
        stretchProg_.store(0);
        const bool ok = (j.repitch ? TimeStretch::repitch : TimeStretch::run)(in, j.outFrames, work,
            [](void* c, uint32_t frame, int16_t* dst, uint32_t n) {
                auto* x = static_cast<Ctx*>(c);
                x->r->read(x->offset + frame, dst, n);
                return true;
            },
            [](void* c, const int16_t* src, uint32_t n) { return static_cast<Ctx*>(c)->w->write(src, n); },
            [](void* c, uint32_t done, uint32_t total) {
                /* The streamer and the screen first: a breath every few hops,
                 * and out at once when anything else is asked of song_io. */
                auto* x = static_cast<Ctx*>(c);
                x->self->stretchProg_.store((uint8_t)((uint64_t)done * 100 / (total ? total : 1)));
                if ((++x->hops & 3) == 0) x->self->p.sleepMs(1);
                if ((x->hops & 15) == 0) x->self->p.yieldIo();
                return !x->self->requestWaiting();
            },
            &ctx);
        p.freeRingSegment(work);
        r.close();
        const bool closed = w.close();
        if (!ok || !closed || !fsx::rename(tmp, j.dst)) {
            std::remove(tmp.c_str());
            if (ok) skip("card write failed");   // stopped by a request: tried again after it
            return;
        }
        logf(false, "%s %s x%.3f", j.repitch ? "repitched" : "stretched", j.dst.c_str(), ratio);
        ready();
    }

    /// Worker: anything asked for that goes before a waveform.
    bool requestWaiting() const {
        return quit_.load() || releaseReq_.load() || stopReq_.load() || playReq_.load() || openReq_.load() ||
               editReq_.load() != 0 || listReq_.load() != 0 || bounceReq_.load() ||
               (saveReq_.load() && p.nowUs() >= saveAtUs_.load());
    }

    bool peaksPending() {
        std::lock_guard<std::mutex> l(peaksMutex_);
        return !peaksWanted_.empty();
    }

    /// Worker: the first waveform asked for -- from .peaks/ when that was
    /// made from the same file, else read from the WAV and kept there. A
    /// request coming in stops the reading; it is started again after.
    void doPeaks() {
        std::string dir, file;
        {
            std::lock_guard<std::mutex> l(peaksMutex_);
            if (peaksWanted_.empty()) return;
            dir = peaksDir_;
            file = peaksWanted_.front();
        }
        auto values = std::make_shared<std::vector<uint8_t>>();
        const std::string wav = dir + "/" + AudioSong::clipsDir() + "/" + file;
        const std::string cache = dir + "/" + kPeaksDir + "/" + file + ".pk";
        uint64_t bytes = 0;
        int64_t modified = 0;
        /* No file: the clip is still cached, with nothing in it, so it is not
         * read for again on every frame -- but it is remembered as missing,
         * which is a different thing from a clip that is simply quiet. A song
         * folder that has been through another computer comes back without some
         * of its audio, and a screen has to be able to say so. */
        const bool here = fsx::fileInfo(wav, bytes, modified);
        if (!here) logf(false, "clip %s has no file in clips/", file.c_str());
        if (here && !loadPeaks(cache, bytes, *values)) {
            if (!readPeaks(wav, *values)) return;
            if (!values->empty()) savePeaks(dir, cache, bytes, *values);
        }
        {
            std::lock_guard<std::mutex> l(peaksMutex_);
            if (dir != peaksDir_) return;           // another project opened meanwhile
            peaks_[file] = std::move(values);
            if (here) peaksMissing_.erase(file);
            else      peaksMissing_.insert(file);
            peaksWanted_.erase(std::remove(peaksWanted_.begin(), peaksWanted_.end(), file), peaksWanted_.end());
        }
        peaksGen_.fetch_add(1);
    }

    /// The loudest sample of every kPeakHop frames of @p wav. False when a
    /// request came in first; a file the engine cannot play gives none.
    bool readPeaks(const std::string& wav, std::vector<uint8_t>& out) {
        ClipReader r;
        if (!r.open(wav)) return true;
        std::vector<int16_t> buf(kPeakChunkFrames * 2);
        out.reserve((r.frames + kPeakHop - 1) / kPeakHop);
        int peak = 0;
        uint32_t inHop = 0;
        for (uint32_t f = 0; f < r.frames; f += kPeakChunkFrames) {
            if (requestWaiting()) return false;
            const uint32_t n = std::min(kPeakChunkFrames, r.frames - f);
            r.read(f, buf.data(), n);
            for (uint32_t i = 0; i < n * 2; i += 2) {
                peak = std::max({peak, std::abs((int)buf[i]), std::abs((int)buf[i + 1])});
                if (++inHop == kPeakHop) {
                    out.push_back((uint8_t)std::min(255, peak >> 7));
                    peak = 0;
                    inHop = 0;
                }
            }
            p.yieldIo();
        }
        if (inHop) out.push_back((uint8_t)std::min(255, peak >> 7));
        return true;
    }

    /* A .pk file: "CPPK", version, the hop, the WAV's size in bytes when it
     * was made, the count, the values. Little-endian. */
    static constexpr uint8_t kPeaksVersion = 1;
    static constexpr size_t  kPeaksHeader  = 16;

    bool loadPeaks(const std::string& path, uint64_t wavBytes, std::vector<uint8_t>& out) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        uint8_t h[kPeaksHeader];
        bool ok = std::fread(h, 1, sizeof(h), f) == sizeof(h) && !std::memcmp(h, "CPPK", 4) &&
                  h[4] == kPeaksVersion && (uint32_t)(h[6] | h[7] << 8) == kPeakHop &&
                  (h[8] | h[9] << 8 | h[10] << 16 | (uint32_t)h[11] << 24) == (uint32_t)wavBytes;
        if (ok) {
            const size_t want = h[12] | h[13] << 8 | h[14] << 16 | (size_t)h[15] << 24;
            /* Against the cache file's own length, not only against the header:
             * a .pk that was truncated or came back damaged from another
             * computer would otherwise ask for a resize of whatever four bytes
             * happened to be there. */
            std::fseek(f, 0, SEEK_END);
            const long len = std::ftell(f);
            std::fseek(f, (long)kPeaksHeader, SEEK_SET);
            const size_t have = len > (long)kPeaksHeader ? (size_t)(len - (long)kPeaksHeader) : 0;
            ok = want && want <= have;
            if (ok) {
                out.resize(want);
                ok = std::fread(out.data(), 1, out.size(), f) == out.size();
            }
        }
        std::fclose(f);
        if (!ok) out.clear();
        return ok;
    }

    void savePeaks(const std::string& dir, const std::string& path, uint64_t wavBytes,
                   const std::vector<uint8_t>& v) {
        if (!fsx::makeDir(dir + "/" + kPeaksDir)) return;
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return;
        const uint32_t b = (uint32_t)wavBytes, n = (uint32_t)v.size();
        const uint8_t h[kPeaksHeader] = {'C', 'P', 'P', 'K', kPeaksVersion, 0,
                                         (uint8_t)kPeakHop, (uint8_t)(kPeakHop >> 8),
                                         (uint8_t)b, (uint8_t)(b >> 8), (uint8_t)(b >> 16), (uint8_t)(b >> 24),
                                         (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
        const bool ok = std::fwrite(h, 1, sizeof(h), f) == sizeof(h) &&
                        std::fwrite(v.data(), 1, v.size(), f) == v.size();
        if (std::fclose(f) != 0 || !ok) std::remove(path.c_str());   // half a file would read as whole
    }

    /// Worker: the live transport -- the clip launcher's. No arrangement:
    /// each track plays whatever was launched on it, looped on the song's bar
    /// grid, and the position simply runs. The cycle is a whole number of
    /// bars just under where the sample counter wraps, which is a day at any
    /// tempo, so a loop's phase is plain subtraction and nothing has to be
    /// re-anchored while anybody is playing.
    void startLive() {
        /* A launch made with the transport stopped is what starts it, so the
         * request it queued has to survive the stop endPlayback() performs
         * here -- otherwise the grid starts with nothing on it. */
        LiveReq keep[kTracks];
        {
            std::lock_guard<std::mutex> l(liveMutex_);
            for (int t = 0; t < kTracks; ++t) keep[t] = liveReq_[t];
        }
        endPlayback();
        {
            std::lock_guard<std::mutex> l(liveMutex_);
            for (int t = 0; t < kTracks; ++t) liveReq_[t] = keep[t];
        }
        uint32_t spb = 0;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            spb = song_.samplesPerBar(sampleRate_);
            /* The song's faders and mutes are the launcher's too; they are
             * only published on a commit, and a project that has had none
             * since it was opened would play at gain zero. */
            for (int t = 0; t < kTracks; ++t) {
                const auto& tr = song_.tracks[t];
                gain_[t].store(song_.silenced(t) ? 0.0f : tr.volume);
            }
        }
        if (!spb) return;
        if (!ring_.alloc(kSongRingFrames, kPrimeFrames)) {
            logf(true, "live: not enough memory for the song ring (%u kB free)",
                 (unsigned)(p.freeMemoryBytes() / 1024));
            return;
        }
        meterReset();
        liveSpb_.store(spb);
        liveCycle_.store((0xFFFF0000u / spb) * spb);
        writePos_ = 0;
        debt_.store(0);
        pos_.store(0);
        startPos_ = 0;
        end_.store(liveCycle_.load());
        loop_.store(true);
        loopA_.store(0);
        loopB_.store(0);
        spBar_.store(spb);
        syncing_ = false;
        take_ = SyncTake::None;
        cinPlanned_.store(0);
        countInReq_.store(0);
        underruns_.store(0);
        openFailures_.store(0);
        played_.store(0);
        liveOn_.store(true);
        job_.store(Job::Play);
        play_.store(Play::Priming, std::memory_order_seq_cst);
    }

    /// Worker: a launch or a stop takes effect, on sample @p at. Caller holds
    /// liveMutex_.
    void liveApply(int t, uint32_t at) {
        LiveReq& r = liveReq_[t];
        if (!r.set) return;
        r.set = false;
        if (r.stop || r.path.empty()) { liveCur_[t] = LiveSlot{}; return; }
        liveCur_[t].path    = r.path;
        liveCur_[t].name    = r.name;
        liveCur_[t].bars    = r.bars ? r.bars : 1;
        liveCur_[t].loop    = liveCur_[t].bars * liveSpb_.load();
        liveCur_[t].startAt = at;
    }

    /// Worker: from the snapshot, @p fromBar on. In sync the sequencer is the
    /// song's: at the song's tempo (its pattern is rescaled with it, so a take
    /// keeps its place in the bar), and stopped until the song starts it.
    void startPlayback(uint32_t fromBar) {
        endPlayback();
        bool sync = false;
        float bpm = 0.0f;
        int seqScene = -1;
        {
            /* A seq track is the sequencer playing its scene with the song --
             * SYNC on or off; SYNC on without one plays the scene in use. */
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            /* A take asked for with the song (the seq view of a track) runs
             * the sequencer even with the track muted: it is being recorded. */
            const bool take = playTake_.load() != SyncTake::None || seqTake_.load() != SyncTake::None;
            const int t = seqTrackOf(song_, take);
            if (t >= 0) seqScene = song_.tracks[t].scene;
            sync = song_.syncSeq || seqScene >= 0 || take;
            bpm = song_.bpm;
        }
        {
            /* The faders and mutes, published before the worker reads them.
             * fillRings() skips a track whose gain is <= 0 and gain_ starts
             * at zero, so a project that has had no commit since it was
             * opened filled the ring with silence: the position advanced, the
             * ring never underran and no file was ever opened, which is
             * exactly what the Arranger looked like -- a playhead moving over
             * a song nobody could hear. The live transport already did this
             * (see liveStart) and said why; this path did not. */
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            for (int t = 0; t < kTracks; ++t) {
                const auto& tr = song_.tracks[t];
                gain_[t].store(song_.silenced(t) ? 0.0f : tr.volume);
            }
        }
        {
            std::lock_guard<std::mutex> l(snapMutex_);
            if (!snap_.end && !snap_.loopB) return;
            const uint32_t from = std::min(fromBar * snap_.samplesPerBar,
                                           std::max(snap_.end, snap_.loopB) - 1);
            if (!ring_.alloc(kSongRingFrames, kPrimeFrames)) {
                logf(true, "play: not enough memory for the song ring (%u kB free)",
                     (unsigned)(p.freeMemoryBytes() / 1024));
                return;
            }
            meterReset();
            writePos_ = from;
            debt_.store(0);
            pos_.store(from);
            startPos_ = from;
        }
        syncing_ = sync;
        take_ = sync ? playTake_.load() : SyncTake::None;
        /* Asked for with this play only; a plain play after it counts none. */
        const uint8_t beats = countInReq_.exchange(0);
        cinPlanned_.store(beats);
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            cinBpb_ = song_.beatsPerBar ? song_.beatsPerBar : AudioSong::kDefaultBeatsPerBar;
            cinBeat_ = song_.samplesPerBar(sampleRate_) / cinBpb_;
        }
        if (sync) {
            PatternSequencer& seq = seq_;
            seq.stop();
            if (bpm > 0.0f && std::fabs(seq.getClickBpm() - bpm) > 0.01f) seq.setClickBpm(bpm);
            /* Stopped, the selection takes at once. */
            if (seqScene >= 0) seq.selectScene((uint8_t)seqScene);
        }
        underruns_.store(0);
        openFailures_.store(0);
        played_.store(0);
        job_.store(Job::Play);
        play_.store(Play::Priming, std::memory_order_seq_cst);
    }

    void endPlayback() {
        lastFillUs_ = 0;
        /* A take this playback ran lands before the sequencer stops: where
         * the song is when it stops is where the take ends. */
        if (takeBound_) {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            finishTake();
            takeBound_ = false;
        }
        if (play_.load() == Play::Idle && job_.load() != Job::Play) return;
        play_.store(Play::Idle, std::memory_order_seq_cst);
        /* The song took the sequencer along; it takes it back down. */
        if (syncing_) {
            seq_.stop();
            seq_.clearSongMap();
        }
        syncing_ = false;
        waitRtOut();
        played_.store(0);
        liveOn_.store(false);
        {
            /* Neither what played nor what was launched outlives a stop: a
             * clip left in a request would fire on the next start. */
            std::lock_guard<std::mutex> l(liveMutex_);
            for (int t = 0; t < kTracks; ++t) { liveCur_[t] = LiveSlot{}; liveReq_[t] = LiveReq{}; }
        }
        for (auto& r : readers_) r.close();
        ring_.release();
        job_.store(Job::None);
    }

    /// Keep the song ring full: each chunk of song time is every unmuted
    /// track's blocks read from the card and summed at its gain. What is not
    /// under a block is silence and costs no read.
    void fillRings() {
        int16_t* chunk = io_;
        int32_t* acc = acc_;
        if (!chunk || !acc) return;
        {
            /* Time off the ring: what the worker did between two top-ups. */
            const int64_t now = p.nowUs();
            if (lastFillUs_) dbgMax(dbgGapMaxUs_, now - lastFillUs_);
            lastFillUs_ = now;
        }
        /* An edit while it plays moves the seq blocks with the audio. */
        if (syncing_ && play_.load() != Play::Priming && snapGen_.load() != mapSentGen_) pushSeqMap();
        while (ring_.space() >= kChunkFrames) {
            if (stopReq_.load()) return;
            uint32_t wp = writePos_;
            uint32_t n = kChunkFrames;
            /* Where each track's blocks fall in this chunk -- a block is at
             * least a bar, so two at most, and a third for safety. The path
             * is copied into the piece rather than a string: this runs every
             * 20 ms, and small heap blocks come from internal RAM. */
            struct Piece { char path[160]; uint32_t at, len, clipFrame; };
            Piece pieces[kTracks][3];
            int pieceCount[kTracks] = {};
            if (liveOn_.load()) {
                /* The launcher: the grid runs on by itself and every track
                 * reads its clip at the phase the position puts it at. A
                 * launch waiting for a line cuts the chunk exactly there, so
                 * the next one starts with the new clips -- which is what
                 * makes a whole row land on one sample. */
                std::lock_guard<std::mutex> l(liveMutex_);
                const uint32_t cycle = liveCycle_.load();
                const uint32_t spb = liveSpb_.load();
                if (!cycle || !spb) return;
                wp += debt_.exchange(0);
                if (wp >= cycle) wp %= cycle;
                n = std::min(n, cycle - wp);
                const uint8_t qb = liveQuant_.load();
                const uint32_t q = (qb ? qb : 1) * spb;
                bool waiting = false;
                for (int t = 0; t < kTracks; ++t) waiting = waiting || liveReq_[t].set;
                if (waiting) {
                    if (wp % q == 0) for (int t = 0; t < kTracks; ++t) liveApply(t, wp);
                    else n = std::min(n, q - wp % q);
                }
                for (int t = 0; t < kTracks; ++t) {
                    LiveSlot& c = liveCur_[t];
                    if (!c.loop || gain_[t].load(std::memory_order_relaxed) <= 0.0f) continue;
                    if (c.startAt > wp) c.startAt = wp;        // once a day, at the wrap
                    uint32_t off = (wp - c.startAt) % c.loop;
                    uint32_t at = 0, left = n;
                    while (left && pieceCount[t] < 3) {
                        const uint32_t run = std::min(left, c.loop - off);
                        Piece& pc = pieces[t][pieceCount[t]++];
                        std::snprintf(pc.path, sizeof(pc.path), "%s", c.path.c_str());
                        pc.at = at;
                        pc.len = run;
                        pc.clipFrame = off;
                        at += run;
                        left -= run;
                        off = 0;
                    }
                }
            } else {
                std::lock_guard<std::mutex> l(snapMutex_);
                const uint32_t end = snap_.end;
                const uint32_t lim = snap_.loop && snap_.loopB > snap_.loopA ? snap_.loopB : end;
                if (!lim) return;
                /* Skip what RT already played as silence. */
                wp += debt_.exchange(0);
                if (wp >= lim) {
                    if (!snap_.loop) { writePos_ = lim; break; }
                    wp = wrap_position(wp, end, snap_.loopA, snap_.loopB);
                }
                n = std::min(n, lim - wp);
                for (int t = 0; t < kTracks; ++t) {
                    if (gain_[t].load(std::memory_order_relaxed) <= 0.0f) continue;
                    for (const auto& sg : snap_.tracks[t]) {
                        if (sg.end <= wp) continue;
                        if (sg.start >= wp + n || pieceCount[t] == 3) break;
                        const uint32_t a = std::max(sg.start, wp);
                        const uint32_t b = std::min(sg.end, wp + n);
                        Piece& pc = pieces[t][pieceCount[t]++];
                        std::snprintf(pc.path, sizeof(pc.path), "%s", sg.path.c_str());
                        pc.at = a - wp;
                        pc.len = b - a;
                        pc.clipFrame = a - sg.start + sg.offset;
                    }
                }
            }
            std::memset(acc, 0, n * 8);
            /* Where this chunk's first frame lands in the ring, which is how
             * RT will count it when it plays it: the meters' clock. */
            const uint32_t at0 = ring_.head.load(std::memory_order_relaxed);
            for (int t = 0; t < kTracks; ++t) {
                /* A track with nothing under this chunk gives its file back:
                 * the sample player shares the card's handful of descriptors,
                 * and a pad that cannot open its sample goes silent. */
                if (!pieceCount[t] && readers_[t].fd >= 0) readers_[t].close();
                const float g = gain_[t].load(std::memory_order_relaxed);
                for (int i = 0; i < pieceCount[t]; ++i) {
                    const Piece& pc = pieces[t][i];
                    const int64_t r0 = p.nowUs();
                    if (readers_[t].path != pc.path &&
                        !(p.storageReady() && readers_[t].open(std::string(pc.path)))) {
                        openFailures_.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    readers_[t].read(pc.clipFrame, chunk, pc.len);
                    /* One card access (an open included): a long one is the
                     * card held by someone else. */
                    dbgMax(dbgReadMaxUs_, p.nowUs() - r0);
                    int32_t* d = acc + pc.at * 2;
                    for (uint32_t k = 0; k < pc.len;) {
                        const uint32_t f = at0 + pc.at + k;
                        const uint32_t run = std::min(pc.len - k, kMeterHop - f % kMeterHop);
                        int32_t m[2] = {0, 0};
                        for (uint32_t j = k * 2; j < (k + run) * 2; ++j) {
                            const int32_t v = (int32_t)((float)chunk[j] * g);
                            d[j] += v;
                            m[j & 1] = std::max(m[j & 1], v < 0 ? -v : v);
                        }
                        meterPut(t, f / kMeterHop, m);
                        k += run;
                    }
                    /* The card is the sampler's streamer's first -- while this
                     * ring has half of itself in hand. Below that it is the
                     * song that is about to run dry: yielding up to 3 ms a
                     * read with voices sounding, three tracks on a 1-bit card
                     * read slower than they played (ringmin 2848 frames,
                     * 09.10.2026). */
                    if (ring_.used() >= ring_.cap / 2) p.yieldIo();
                }
            }
            for (uint32_t k = 0; k < n * 2; ++k) {
                const int32_t v = acc[k];
                chunk[k] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            }
            ring_.push(chunk, n);
            writePos_ = wp + n;
        }
        if (play_.load() == Play::Priming && ring_.used() >= kPrimeFrames) {
            if (syncing_) {
                /* The pattern laid on the song's bars: the scene's bar one on
                 * the song's bar one, so starting mid-song starts mid-scene
                 * where the timeline says. RT starts the song with the clock. */
                const uint32_t at = startPos_;
                PatternSequencer& seq = seq_;
                MetronomeNode& clk = clk_;
                const uint32_t loop = clk.loopSamples();
                armPos_.store(loop ? at % loop : 0, std::memory_order_relaxed);
                /* Before the clock moves: a poll in between would fire a
                 * window of the scene the blocks say is silent. */
                pushSeqMap();
                play_.store(Play::Armed, std::memory_order_release);
                /* A count-in is the metronome's: the scene waits in PreBeat
                 * and the song for the clock to move, both on its last click. */
                seq.playbackAt(at, cinPlanned_.load());
                /* A take asked for with the song: opened with it, on the same
                 * sample, and never with a count-in of its own. */
                if (take_ == SyncTake::Record)       seq.record();
                else if (take_ == SyncTake::Overdub) seq.overdub();
            } else if (const uint8_t beats = cinPlanned_.load()) {
                /* Alone the song counts itself in: the same click, on the
                 * song's beat, and the song on the sample after the last. */
                cinTotal_ = cinLeft_ = beats * cinBeat_;
                clickLen_ = sampleRate_ / 50;
                clickLeft_ = 0;
                cinBeatsLeft_.store(beats, std::memory_order_relaxed);
                play_.store(Play::Counting, std::memory_order_release);
            } else {
                play_.store(Play::Running, std::memory_order_release);
            }
        }
    }

    void runBounce() {
        auto fail = [this](const char* why) {
            logf(true, "bounce failed: %s", why);
            setStatus(BounceState::Failed, why);
        };
        PatternSequencer& seq = seq_;
        MetronomeNode& clk = clk_;

        endPlayback();                           // a bounce owns the transport
        if (!opened_) { fail("no card"); return; }
        if (bScene_ >= seq.sceneCount()) { fail("no such scene"); return; }
        if (!p.storageReady()) { fail("card or memory unavailable"); return; }

        /* <kit>_S<scene>_<n>.wav, the first n that is free. */
        std::string kit = "seq";
        if (auto* km = getKitManager()) {
            if (auto* k = km->getCurrentKit()) kit = k->name;
        }
        for (auto& ch : kit) if (!isalnum((unsigned char)ch)) ch = '_';
        /* Short enough that "_S<scene>_<nn>.wav" always fits after it: cut
         * off, the counter went and every bounce after the first reused one
         * name and wrote over that clip. */
        if (kit.size() > kMaxClipPrefix) kit.resize(kMaxClipPrefix);
        std::string file, path;
        for (int n = 1; n < 1000; ++n) {
            char buf[80];
            std::snprintf(buf, sizeof(buf), "%s_S%u_%02d.wav", kit.c_str(), (unsigned)(bScene_ + 1), n);
            file = buf;
            {
                std::lock_guard<std::recursive_mutex> l(songMutex_);
                path = song_.clipPath(file);
            }
            if (!fsx::exists(path)) break;
        }

        WavWriter w;
        if (!io_) { fail("not enough memory"); return; }
        if (!w.open(path.c_str(), sampleRate_, 2)) { fail("card write failed"); return; }
        /* A sixth of a second of card stall is the least worth recording
         * with; a card that stalls longer than the ring holds fails the take
         * as "card too slow" rather than writing it with holes. */
        if (!bRing_.alloc(kBounceRingFrames, kBounceMinFrames)) {
            w.close();
            std::remove(path.c_str());
            char why[64];
            std::snprintf(why, sizeof(why), "not enough memory (%u kB free)",
                          (unsigned)(p.freeMemoryBytes() / 1024));
            fail(why);
            return;
        }

        /* The scene, once, from its bar one: no count-in, and the pads bus at
         * unity (the platform's). All of it is the player's and goes back as
         * it was. */
        const bool countIn = seq.countIn();
        const uint8_t sceneWas = seq.activeScene();
        auto restore = [&] {
            seq.stop();
            seq.clearSongMap();
            seq.setCountIn(countIn);
            seq.selectScene(sceneWas);
            p.endPadsCapture();
        };
        seq.stop();
        seq.setCountIn(false);
        p.beginPadsCapture();
        seq.selectScene(bScene_);

        /* One pass of the scene, or -- a seq track's bounce -- the scene
         * looping over the song's bars, which is what was heard. */
        const uint32_t bars = bBars_ ? bBars_ : (seq.sceneBars(bScene_) ? seq.sceneBars(bScene_) : 1);
        const uint32_t total = clk.samplesPerBeat() * clk.beatsPerBar() * bars;
        if (!w.reserve(total)) {
            restore();
            bRing_.release();
            w.close();
            std::remove(path.c_str());
            fail("card full");
            return;
        }
        bBpm_ = seq.getClickBpm();
        /* A seq track cut into blocks is bounced as it plays: the scene only
         * under them, on the clock's bars from the song's bar one. */
        if (bBars_) {
            std::vector<PatternSequencer::SongSpan> spans;
            bool mapped = false;
            {
                std::lock_guard<std::recursive_mutex> l(songMutex_);
                const SongTrack& tr = song_.tracks[bTrack_];
                if (tr.kind == TrackKind::Seq && tr.seqBlocks) {
                    spans = seqSpansOf(tr, clk.samplesPerBeat() * clk.beatsPerBar(), song_.seqTicksPerBar(), total);
                    mapped = true;
                }
            }
            if (mapped) seq.setSongMap(spans, 0, 0);
        }
        {
            std::lock_guard<std::mutex> l(statusMutex_);
            status_.framesTotal = total;
            status_.file = file;
        }
        bLeft_ = total;
        bCapturing_ = false;
        bOverflow_.store(false);
        bDone_.store(false);
        bArmed_.store(true, std::memory_order_seq_cst);
        seq.playback();

        int16_t* chunk = io_;
        const int64_t deadline = p.nowUs() +
                                 (int64_t)total * 1000000 / sampleRate_ + 3000000;
        const char* error = nullptr;
        char why[64];
        uint32_t done = 0;
        int64_t maxGapUs = 0, lastPop = 0;   // from the first block on: not the wait for bar one
        bool seqStopped = false;
        for (;;) {
            /* A lost block is a failed take, whatever else is true: checked
             * first, so a ring that overflowed and then drained is never
             * reported as a finished clip. */
            if (bOverflow_.load()) {
                std::snprintf(why, sizeof(why), "card too slow (stalled %u ms, buffer %u ms)",
                              (unsigned)(maxGapUs / 1000),
                              (unsigned)((uint64_t)bRing_.cap * 1000 / sampleRate_));
                error = why;
                break;
            }
            /* The take is complete the moment RT has its last frame: stop the
             * scene there, not when the card has caught up. */
            if (!seqStopped && bDone_.load()) { seq.stop(); seqStopped = true; }
            const uint32_t n = bRing_.pop(chunk, kChunkFrames);
            if (n) {
                if (!w.write(chunk, n)) { error = "card write failed"; break; }
                const int64_t now = p.nowUs();
                if (lastPop) maxGapUs = std::max(maxGapUs, now - lastPop);
                lastPop = now;
                done += n;
                {
                    std::lock_guard<std::mutex> l(statusMutex_);
                    status_.framesDone = done;
                }
                p.yieldIo();
                continue;
            }
            if (bDone_.load() && !bRing_.used()) break;
            if (bCancel_.load()) { error = "cancelled"; break; }
            if (p.nowUs() > deadline) { error = "timed out"; break; }
            p.sleepMs(2);
        }
        if (!error && done != total) error = "take incomplete";

        bArmed_.store(false, std::memory_order_seq_cst);
        restore();
        waitRtOut();
        bRing_.release();
        const bool closed = w.close();

        if (error || !closed) {
            std::remove(path.c_str());
            fail(error ? error : "card write failed");
            return;
        }
        logf(false, "bounced scene %u to %s (%u frames)", (unsigned)(bScene_ + 1), file.c_str(), (unsigned)done);
        /* Onto its track, after the last block. An empty song takes the tempo
         * of the clip it is given -- its grid is only ever as right as its
         * first clip, and a song whose blocks were all deleted is a new start,
         * not a tempo to keep. */
        std::string note;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            if (song_.bpm <= 0.0f || song_.songEnd() == 0) song_.bpm = bBpm_;
            /* Its length on the song's grid, not the scene's bar count: a
             * scene at another tempo is a different number of the song's
             * bars, and a block sized by the scene cut the end of the take
             * off (95 BPM in a 120 BPM song lost a fifth of it). */
            const uint32_t spBar = song_.samplesPerBar(sampleRate_);
            const int target = bBars_ ? (int)bTrack_ : audioTarget(bTrack_, true);
            if (target < 0) {
                std::remove(path.c_str());
                fail("no audio track to put it on");
                return;
            }
            bTrack_ = (uint8_t)target;
            SongClip c;
            c.file = file;
            /* A seq track's bounce is the song from its bar one; a scene's
             * goes after the track's last block. */
            c.bar  = bBars_ ? 0 : song_.trackEnd(bTrack_);
            c.bars = std::max<uint32_t>(1, (done + spBar - 1) / spBar);
            song_.addClip(bTrack_, c);
            /* A seq track's bounce: its notes are this clip now, and the slot
             * is free for the next idea. */
            if (bBars_) song_.bounceSeqTrack(bTrack_);
            rebuild();
            if (std::fabs(song_.bpm - bBpm_) >= 0.5f) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "scene at %d BPM, song at %d BPM: off the bar grid",
                              (int)lroundf(bBpm_), (int)lroundf(song_.bpm));
                note = buf;
            }
        }
        doSave();
        setStatus(BounceState::Done, note);
    }

    // Song model and its flattened copy (worker).
    AudioSong   song_;
    std::recursive_mutex songMutex_;
    std::atomic<bool> opened_{false};
    bool        lastTried_ = false;            // worker only
    std::string openName_;
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
    uint16_t    takeLaps_ = 0;
    bool        takeReplaced_ = false;
    std::vector<PatternSequencer::SongTakeNote> takeNotes_;
    bool        takeHorizon_ = false;
    bool        takeBound_ = false;              // worker: the playback running is the take's
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
                             kEditDelete = 5, kEditExport = 6;
    uint32_t    importA_ = 0, importB_ = 0, importBar_ = 0, importLead_ = 0;   // set before editReq_
    uint8_t     importTrack_ = 0;
    std::atomic<uint8_t>  editMask_{0}, editProgress_{0};
    std::atomic<uint32_t> editFrom_{0}, editTo_{0};
    std::atomic<uint8_t>  editReq_{0};
    std::atomic<SongOpState> editState_{SongOpState::Idle};
    std::atomic<float>    editBpm_{120.0f};
    std::string editName_;                        // under songMutex_
    std::vector<std::string> report_;             // the last export's, under reportMutex_
    std::mutex  reportMutex_;
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
    std::vector<std::string> projects_;
    std::vector<ClipInfo>    clips_;
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
    uint32_t    writePos_ = 0;
    std::atomic<uint32_t> debt_{0};
    std::atomic<float>    gain_[kTracks] = {};
    /// One hop of the ring's meters: which hop it holds, and each track's peak.
    struct Meter {
        std::atomic<uint32_t> hop{UINT32_MAX};
        std::atomic<uint16_t> peak[kTracks][2] = {};
    };
    Meter                 meter_[kMeterSlots];
    std::atomic<float>    padsPeak_[2] = {};
    std::atomic<int8_t>   seqTrack_{-1};
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
    std::atomic<uint32_t> stretchCount_{0};
    std::atomic<uint8_t>  stretchProg_{0};   ///< the block being fitted, 0..100

    // Undo: steps back and forward, and the arrangement as last seen (all
    // under songMutex_); the counts for the screen.
    std::vector<Snapshot> undo_, redo_;
    Snapshot              last_;
    int64_t               lastChangeUs_ = 0;
    std::atomic<uint32_t> undoCount_{0}, redoCount_{0};
};

SongEngine::SongEngine(ISongEnginePlatform& platform, MetronomeNode& clock, PatternSequencer& seq)
    : impl_(std::make_unique<Impl>(platform, clock, seq)) {}

SongEngine::~SongEngine()
{
    impl_->shutdown();
}

const char* SongEngine::name() const { return "Song"; }
bool SongEngine::isGenerator() const { return true; }
void SongEngine::onPrepare(uint32_t maxFrames, uint32_t sr) { impl_->onPrepare(maxFrames, sr); }
void SongEngine::onSampleRateChange(uint32_t sr) { impl_->onSampleRateChange(sr); }
void SongEngine::mix(float* out, uint32_t frames) { impl_->mix(out, frames); }
void SongEngine::rtCapture(const float* padsBus, uint32_t frames) { impl_->rtCapture(padsBus, frames); }
void SongEngine::releaseRings() { impl_->releaseRings(); }
const char* SongEngine::debugLine() const { return impl_->debugLine(); }
bool SongEngine::liveStart() { return impl_->liveStart(); }
bool SongEngine::liveLaunch(uint8_t track, const std::string& file, uint16_t bars)
{ return impl_->liveLaunch(track, file, bars); }
bool SongEngine::liveStop(uint8_t track) { return impl_->liveStop(track); }
void SongEngine::liveQuant(uint8_t bars) { impl_->liveQuant(bars); }
bool SongEngine::liveState(LiveState& out) { return impl_->liveState(out); }
bool SongEngine::requestProjects() { return impl_->requestProjects(); }
std::vector<std::string> SongEngine::projects() { return impl_->projects(); }
bool SongEngine::requestOpen(const std::string& name) { return impl_->requestOpen(name); }
SongOpState SongEngine::openState() const { return impl_->openState(); }
AudioSong& SongEngine::song() { return impl_->song(); }
std::recursive_mutex& SongEngine::songMutex() { return impl_->songMutex(); }
void SongEngine::commit() { impl_->commit(); }
bool SongEngine::requestClips() { return impl_->requestClips(); }
std::vector<ClipInfo> SongEngine::clips() { return impl_->clips(); }
uint32_t SongEngine::listGeneration() const { return impl_->listGeneration(); }
bool SongEngine::bounceScene(uint8_t scene, uint8_t track) { return impl_->bounceScene(scene, track); }
bool SongEngine::bounceSeqTrack(uint8_t track) { return impl_->bounceSeqTrack(track); }
uint32_t SongEngine::playEndBars() { return impl_->playEndBars(); }
BounceStatus SongEngine::bounceStatus() { return impl_->bounceStatus(); }
void SongEngine::cancelBounce() { impl_->cancelBounce(); }
bool SongEngine::requestRename(const std::string& name) { return impl_->requestRename(name); }
bool SongEngine::requestDelete() { return impl_->requestDelete(); }
bool SongEngine::requestTempo(float bpm) { return impl_->requestTempo(bpm); }
SongOpState SongEngine::editState() const { return impl_->editState(); }
bool SongEngine::requestMerge(uint8_t mask, uint32_t fromBar, uint32_t toBar) { return impl_->requestMerge(mask, fromBar, toBar); }
uint8_t SongEngine::editProgress() const { return impl_->editProgress(); }
uint32_t SongEngine::stretchWaiting() const { return impl_->stretchWaiting(); }
void SongEngine::trackLevels(int track, float& left, float& right) { impl_->trackLevels(track, left, right); }
bool SongEngine::undo() { return impl_->undo(); }
bool SongEngine::redo() { return impl_->redo(); }
uint32_t SongEngine::undoSteps() const { return impl_->undoSteps(); }
uint32_t SongEngine::redoSteps() const { return impl_->redoSteps(); }
uint8_t SongEngine::stretchProgress() const { return impl_->stretchProgress(); }
bool SongEngine::requestExportLiveSet() { return impl_->requestExportLiveSet(); }
std::vector<std::string> SongEngine::editReport() { return impl_->editReport(); }
bool SongEngine::clipPeaks(const std::string& file, ClipPeaks& out) { return impl_->clipPeaks(file, out); }
uint32_t SongEngine::peaksGeneration() const { return impl_->peaksGeneration(); }
uint32_t SongEngine::framesPerBar() { return impl_->framesPerBar(); }
bool SongEngine::requestImport(const std::string& file, uint32_t start, uint32_t end,
                               uint8_t track, uint32_t bar, uint32_t lead)
{
    return impl_->requestImport(file, start, end, track, bar, lead);
}
bool SongEngine::play(uint32_t fromBar) { return impl_->playWithTake(fromBar, SyncTake::None); }
bool SongEngine::playWithTake(uint32_t fromBar, SyncTake take) { return impl_->playWithTake(fromBar, take); }
bool SongEngine::playCountedIn(uint32_t fromBar, SyncTake take, uint8_t beats)
{
    return impl_->playCountedIn(fromBar, take, beats);
}
uint8_t SongEngine::countInBeatsLeft() const { return impl_->countInBeatsLeft(); }
void SongEngine::stop() { impl_->stop(); }
bool SongEngine::playing() const { return impl_->playing(); }
double SongEngine::positionBars() const { return impl_->positionBars(); }
uint32_t SongEngine::framesPlayed() const { return impl_->framesPlayed(); }
uint32_t SongEngine::positionFrames() const { return impl_->positionFrames(); }
uint32_t SongEngine::underruns() const { return impl_->underruns(); }
uint32_t SongEngine::openFailures() const { return impl_->openFailures(); }
bool SongEngine::recordSeq(uint32_t fromBar, SyncTake take, uint8_t beats) { return impl_->recordSeq(fromBar, take, beats); }
bool SongEngine::seqPunch(SyncTake take) { return impl_->seqPunch(take); }
SyncTake SongEngine::seqTake() const { return impl_->seqTake(); }
bool SongEngine::seqPunchable() const { return impl_->seqPunchable(); }
bool SongEngine::openSeqEdit(uint8_t track, uint32_t bar, uint8_t scene) { return impl_->openSeqEdit(track, bar, scene); }
void SongEngine::syncSeqEdit() { impl_->syncSeqEdit(); }
void SongEngine::closeSeqEdit() { impl_->closeSeqEdit(); }
int32_t SongEngine::seqEditBar() const { return impl_->seqEditBar(); }

} // namespace crosspad

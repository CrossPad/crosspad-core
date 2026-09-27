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
#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/kit/KitInfo.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sequencer/MetronomeNode.hpp"
#include "crosspad/sequencer/PatternSequencer.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#ifndef O_BINARY
#define O_BINARY 0
#endif

namespace crosspad {
namespace {

constexpr int      kTracks           = AudioSong::kTracks;
constexpr uint32_t kBounceRingFrames = 32768;  // 128 kB, ~0.7 s of card stall
constexpr uint32_t kBounceMinFrames  = 8192;   // 32 kB (~170 ms), the least a bounce starts with
/* The tracks are mixed by the worker into one ring, not played from a ring
 * each: four rings were 128 kB of PSRAM, more than the board has free with an
 * app open, and play simply did not start. The price is that a mute or a
 * track fader is heard a ring's depth late, ~170 ms. */
constexpr uint32_t kSongRingFrames   = 8192;   // 32 kB, ~170 ms ahead
constexpr uint32_t kChunkFrames      = 1024;   // 4 kB per card access
constexpr uint32_t kPrimeFrames      = kSongRingFrames / 2;
/// A project name, as a folder on the card and a row in a list.
constexpr size_t   kMaxNameLen       = 32;
/// The kit's part of a clip's file name.
constexpr size_t   kMaxClipPrefix    = 32;
/// While the card is not there yet, how often the worker looks for it.
constexpr uint32_t kCardPollMs       = 1000;
/// How often the worker tops the song ring up while it plays.
constexpr uint32_t kStreamPollMs     = 3;
/// releaseRings(): how often it logs that it is still waiting (a bounce
/// notices a cancel within one card write).
constexpr uint32_t kReleaseWaitMs    = 3000;
constexpr uint32_t kReleasePollMs    = 10;

/// A name that stays one folder under the songs root.
bool valid_name(const std::string& n)
{
    return !n.empty() && n.size() <= kMaxNameLen && n != "." && n != ".." &&
           n.find_first_of("/\\") == std::string::npos;
}

bool make_dir(const std::string& p)
{
    struct stat st;
    if (stat(p.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(p.c_str(), 0775) == 0;
}

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
    int         fd = -1;
    std::string path;
    uint32_t    dataOffset = 0;
    uint32_t    frames = 0;
    uint16_t    channels = 2;
    uint32_t    cursor = UINT32_MAX;   // frame the file position is at

    void close() { if (fd >= 0) ::close(fd); fd = -1; path.clear(); cursor = UINT32_MAX; }

    bool readAll(void* dst, size_t n) { return ::read(fd, dst, n) == (ssize_t)n; }

    bool open(const std::string& p) {
        close();
        fd = ::open(p.c_str(), O_RDONLY | O_BINARY);
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
                if (size > 16 && ::lseek(fd, (off_t)at, SEEK_SET) < 0) break;
            } else if (!std::memcmp(ch, "data", 4)) {
                if (!fmtOk) break;
                dataOffset = at;
                frames = size / (channels * 2u);
                path = p;
                cursor = 0;
                return true;
            } else {
                at += size + (size & 1);
                if (::lseek(fd, (off_t)at, SEEK_SET) < 0) break;
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
                ::lseek(fd, (off_t)(dataOffset + frame * channels * 2u), SEEK_SET);
                cursor = frame;
            }
            const uint32_t want = std::min(n, frames - frame);
            const uint32_t fb = channels * 2u;
            const ssize_t r = ::read(fd, dst, want * fb);
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
struct Segment { std::string path; uint32_t start = 0, end = 0; };
struct Arrangement {
    std::vector<Segment> tracks[kTracks];
    uint32_t end = 0;             // samples; 0 = empty song
    uint32_t samplesPerBar = 0;
    bool     loop = true;
};

/// Marks the render inside this node: the counter is odd from entry to exit,
/// so the worker can wait for a block in flight before freeing what RT reads.
struct RtSection {
    std::atomic<uint32_t>& gen;
    explicit RtSection(std::atomic<uint32_t>& g) : gen(g) { gen.fetch_add(1, std::memory_order_seq_cst); }
    ~RtSection() { gen.fetch_add(1, std::memory_order_seq_cst); }
};

/* Armed: primed and waiting for the sequencer's clock, which the song starts
 * with when it plays in sync (see startPlayback()). */
enum class Play : uint8_t { Idle, Priming, Armed, Running, Ended };

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
        if (st == Play::Armed) {
            /* The clock is mixed before this node, so the block in which its
             * position first moves off where it was started is the block it
             * started in: the song starts in the same one, to the sample. */
            MetronomeNode& clk = clk_;
            if (!clk.running() || clk.positionSamples() == armPos_.load(std::memory_order_relaxed)) return;
            /* Only from Armed: a stop the worker published since this block
             * began must not be turned back into Running. */
            Play expected = Play::Armed;
            if (!play_.compare_exchange_strong(expected, Play::Running, std::memory_order_seq_cst)) return;
            st = Play::Running;
        }
        if (st != Play::Running) return;
        if (frames * 2 > scratch_.size()) return;
        const uint32_t got = ring_.pop(scratch_.data(), frames);
        if (got < frames) {
            /* The card fell behind. Play silence and tell the worker how far
             * to skip, so the song comes back in time, not late. */
            debt_.fetch_add(frames - got, std::memory_order_relaxed);
            underruns_.fetch_add(1, std::memory_order_relaxed);
        }
        const int16_t* s = scratch_.data();
        for (uint32_t i = 0; i < got * 2; ++i) out[i] += (float)s[i] * (1.0f / 32768.0f);
        uint32_t pos = pos_.load(std::memory_order_relaxed) + frames;
        const uint32_t end = end_.load(std::memory_order_relaxed);
        if (end && pos >= end) {
            /* Modulo, not minus: the song can be shortened while it plays. */
            if (loop_.load(std::memory_order_relaxed)) pos %= end;
            else {
                Play expected = Play::Running;
                play_.compare_exchange_strong(expected, Play::Ended, std::memory_order_seq_cst);
            }
        }
        pos_.store(pos, std::memory_order_relaxed);
    }

    /// The pads bus, one block. Only while a bounce is armed.
    void rtCapture(const float* bus, uint32_t frames) {
        RtSection rt(rtGen_);
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
    bool bounceScene(uint8_t scene, uint8_t track) {
        if (track >= kTracks || !ensureWorker() || busy_.exchange(true)) return false;
        {
            std::lock_guard<std::mutex> l(statusMutex_);
            status_ = BounceStatus{};
            status_.state = BounceState::Running;
        }
        bScene_ = scene;
        bTrack_ = track;
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
    bool play(uint32_t fromBar) {
        if (!ensureWorker() || busy_.load()) return false;           // a bounce owns the transport
        {
            std::lock_guard<std::mutex> l(snapMutex_);
            if (!snap_.end) return false;
        }
        playFrom_.store(fromBar);
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
        return p == Play::Priming || p == Play::Armed || p == Play::Running;
    }

    double positionBars() const {
        const uint32_t spb = spBar_.load();
        return spb ? (double)pos_.load() / spb : 0.0;
    }

    uint32_t underruns() const { return underruns_.load(); }
    uint32_t openFailures() const { return openFailures_.load(); }

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
            if (DIR* d = opendir(root.c_str())) {
                while (dirent* e = readdir(d)) {
                    if (e->d_name[0] == '.') continue;
                    const std::string path = root + "/" + e->d_name;
                    struct stat st;
                    if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) out.push_back(e->d_name);
                    p.yieldIo();
                }
                closedir(d);
            }
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
            if (DIR* d = opendir(dir.c_str())) {
                while (dirent* e = readdir(d)) {
                    const std::string name = e->d_name;
                    if (name.size() < 5 || name.compare(name.size() - 4, 4, ".wav") != 0) continue;
                    /* Its length on the grid from the header alone: a bounce
                     * is a whole number of bars, anything else rounds up. */
                    ClipReader r;
                    if (r.open(dir + "/" + name) && r.frames && spBar) {
                        ClipInfo ci;
                        ci.file = name;
                        const uint32_t bars = (r.frames + spBar - 1) / spBar;
                        ci.bars = (uint16_t)std::max<uint32_t>(1, std::min<uint32_t>(bars, 999));
                        out.push_back(ci);
                    }
                    r.close();
                    p.yieldIo();
                }
                closedir(d);
            }
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
        if (!make_dir(root) || !make_dir(dir) || !make_dir(dir + "/" + AudioSong::clipsDir())) {
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
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            song_ = std::move(fresh);
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

    /// Worker: write the song. A copy is taken under the lock so the card
    /// write does not hold the UI out.
    void doSave() {
        AudioSong copy;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            if (!opened_) return;
            copy = song_;
        }
        if (p.storageReady() && !copy.save()) logf(true, "song.json not written");
    }

    /// The song as sample ranges, for the worker, and the per-track gains
    /// and loop for RT -- so a mute or a fader acts at once while playing.
    /// Caller holds songMutex_.
    void rebuild() {
        Arrangement a;
        a.samplesPerBar = song_.samplesPerBar(sampleRate_);
        a.loop = song_.loop;
        a.end = song_.playEnd() * a.samplesPerBar;
        for (int t = 0; t < kTracks; ++t) {
            const auto& cs = song_.tracks[t].clips;
            for (size_t i = 0; i < cs.size(); ++i) {
                Segment s;
                s.path  = song_.clipPath(cs[i].file);
                s.start = cs[i].bar * a.samplesPerBar;
                s.end   = (cs[i].bar + cs[i].bars) * a.samplesPerBar;
                /* A later block cuts the one it lands on, as on any timeline,
                 * and the song's length cuts everything past it. */
                if (i + 1 < cs.size()) s.end = std::min(s.end, cs[i + 1].bar * a.samplesPerBar);
                s.end = std::min(s.end, a.end);
                if (s.end > s.start) a.tracks[t].push_back(s);
            }
            const auto& tr = song_.tracks[t];
            gain_[t].store(tr.muted ? 0.0f : tr.volume);
        }
        loop_.store(a.loop);
        spBar_.store(a.samplesPerBar);
        {
            std::lock_guard<std::mutex> l(snapMutex_);
            end_.store(a.end);
            snap_ = std::move(a);
        }
    }

    void setStatus(BounceState st, const std::string& err = std::string()) {
        std::lock_guard<std::mutex> l(statusMutex_);
        status_.state = st;
        if (!err.empty()) status_.error = err;
    }

    void worker() {
        while (!quit_.load()) {
            const bool streaming = job_.load() == Job::Play && play_.load() != Play::Idle;
            /* Until a project is open, look for the card once a second. */
            p.waitWake(streaming ? kStreamPollMs : (lastTried_ ? UINT32_MAX : kCardPollMs));
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
            if (playReq_.exchange(false)) {
                startPlayback(playFrom_.load());
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
            if (const uint8_t lists = listReq_.exchange(0)) {
                if (lists & kListProjects) doListProjects();
                if (lists & kListClips)    doListClips();
                listGen_.fetch_add(1);
                continue;
            }
            if (!lastTried_) openLast();
            if (saveReq_.exchange(false)) doSave();
            if (bounceReq_.exchange(false)) {
                runBounce();
                busy_.store(false);
                continue;
            }
            if (job_.load() == Job::Play && play_.load() != Play::Idle) fillRings();
        }
    }

    /// Worker: from the snapshot, @p fromBar on. In sync the sequencer is the
    /// song's: at the song's tempo (its pattern is rescaled with it, so a take
    /// keeps its place in the bar), and stopped until the song starts it.
    void startPlayback(uint32_t fromBar) {
        endPlayback();
        bool sync = false;
        float bpm = 0.0f;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            sync = song_.syncSeq;
            bpm = song_.bpm;
        }
        {
            std::lock_guard<std::mutex> l(snapMutex_);
            if (!snap_.end) return;
            const uint32_t from = std::min(fromBar * snap_.samplesPerBar, snap_.end - 1);
            if (!ring_.alloc(kSongRingFrames, kPrimeFrames)) {
                logf(true, "play: not enough memory for the song ring (%u kB free)",
                     (unsigned)(p.freeMemoryBytes() / 1024));
                return;
            }
            writePos_ = from;
            debt_.store(0);
            pos_.store(from);
            startPos_ = from;
        }
        syncing_ = sync;
        if (sync) {
            PatternSequencer& seq = seq_;
            seq.stop();
            if (bpm > 0.0f && std::fabs(seq.getClickBpm() - bpm) > 0.01f) seq.setClickBpm(bpm);
        }
        underruns_.store(0);
        openFailures_.store(0);
        job_.store(Job::Play);
        play_.store(Play::Priming, std::memory_order_seq_cst);
    }

    void endPlayback() {
        if (play_.load() == Play::Idle && job_.load() != Job::Play) return;
        play_.store(Play::Idle, std::memory_order_seq_cst);
        /* The song took the sequencer along; it takes it back down. */
        if (syncing_) seq_.stop();
        syncing_ = false;
        waitRtOut();
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
            {
                std::lock_guard<std::mutex> l(snapMutex_);
                const uint32_t end = snap_.end;
                if (!end) return;
                /* Skip what RT already played as silence. */
                wp += debt_.exchange(0);
                if (wp >= end) {
                    if (!snap_.loop) { writePos_ = end; break; }
                    wp %= end;
                }
                n = std::min(n, end - wp);
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
                        pc.clipFrame = a - sg.start;
                    }
                }
            }
            std::memset(acc, 0, n * 8);
            for (int t = 0; t < kTracks; ++t) {
                /* A track with nothing under this chunk gives its file back:
                 * the sample player shares the card's handful of descriptors,
                 * and a pad that cannot open its sample goes silent. */
                if (!pieceCount[t] && readers_[t].fd >= 0) readers_[t].close();
                const float g = gain_[t].load(std::memory_order_relaxed);
                for (int i = 0; i < pieceCount[t]; ++i) {
                    const Piece& pc = pieces[t][i];
                    if (readers_[t].path != pc.path &&
                        !(p.storageReady() && readers_[t].open(std::string(pc.path)))) {
                        openFailures_.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    readers_[t].read(pc.clipFrame, chunk, pc.len);
                    int32_t* d = acc + pc.at * 2;
                    for (uint32_t k = 0; k < pc.len * 2; ++k) d[k] += (int32_t)((float)chunk[k] * g);
                    p.yieldIo();
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
                play_.store(Play::Armed, std::memory_order_release);
                seq.playbackAt(at);
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
            struct stat st;
            if (stat(path.c_str(), &st) != 0) break;
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
            seq.setCountIn(countIn);
            seq.selectScene(sceneWas);
            p.endPadsCapture();
        };
        seq.stop();
        seq.setCountIn(false);
        p.beginPadsCapture();
        seq.selectScene(bScene_);

        const uint8_t bars = seq.sceneBars(bScene_) ? seq.sceneBars(bScene_) : 1;
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
            SongClip c;
            c.file = file;
            c.bar  = song_.trackEnd(bTrack_);
            c.bars = std::max<uint32_t>(1, (done + spBar - 1) / spBar);
            song_.addClip(bTrack_, c);
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
    std::atomic<SongOpState> openState_{SongOpState::Idle};
    std::atomic<uint32_t> playFrom_{0};
    std::atomic<uint8_t>  listReq_{0};
    std::atomic<uint32_t> listGen_{0};
    std::vector<std::string> projects_;
    std::vector<ClipInfo>    clips_;
    std::mutex  listMutex_;
    Arrangement snap_;
    std::mutex  snapMutex_;

    // Playback.
    FrameRing   ring_;
    ClipReader  readers_[kTracks];
    uint32_t    writePos_ = 0;
    std::atomic<uint32_t> debt_{0};
    std::atomic<float>    gain_[kTracks] = {};
    std::atomic<Play>     play_{Play::Idle};
    std::atomic<bool>     syncing_{false};         // this playback runs the sequencer
    uint32_t              startPos_ = 0;           // song sample playback started at
    std::atomic<uint32_t> armPos_{0};              // clock position it was started at
    std::atomic<uint32_t> pos_{0}, end_{0}, spBar_{0}, underruns_{0}, openFailures_{0};
    std::atomic<uint32_t> rtGen_{0};               // odd while the render is inside
    std::atomic<bool>     loop_{true};
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
BounceStatus SongEngine::bounceStatus() { return impl_->bounceStatus(); }
void SongEngine::cancelBounce() { impl_->cancelBounce(); }
bool SongEngine::play(uint32_t fromBar) { return impl_->play(fromBar); }
void SongEngine::stop() { impl_->stop(); }
bool SongEngine::playing() const { return impl_->playing(); }
double SongEngine::positionBars() const { return impl_->positionBars(); }
uint32_t SongEngine::underruns() const { return impl_->underruns(); }
uint32_t SongEngine::openFailures() const { return impl_->openFailures(); }

} // namespace crosspad

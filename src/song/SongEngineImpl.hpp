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
#include "crosspad/song/LiveSet.hpp"

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
/// requestTempo(): the tempi a song may have.
constexpr float    kMinBpm           = 20.0f;
constexpr float    kMaxBpm           = 400.0f;
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
/// A freeze's renders, in clips/, named by what they play (pieceKey()).
constexpr const char* kFreezeDir     = ".freeze";
/// Within this of 1:1 a block plays as recorded -- a copy would be the same
/// sound, a card read later.
constexpr double      kStretchSame   = 0.005;

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
    SongOpState openState() const { return openState_.load(); }
    bool requestRename(const std::string& name);
    bool requestDelete();
    bool requestTempo(float bpm);
    SongOpState editState() const { return editState_.load(); }
    bool requestMerge(uint8_t trackMask, uint32_t fromBar, uint32_t toBar);
    uint8_t editProgress() const { return editProgress_.load(); }
    uint32_t stretchWaiting() const { return stretchCount_.load(); }
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
    char dbg_[320] = {};
    /* ASONG_DBG's margins: see debugLine(). */
    std::atomic<uint32_t> dbgRingMin_{UINT32_MAX};
    std::atomic<uint32_t> dbgGapMaxUs_{0};
    std::atomic<uint32_t> dbgReadMaxUs_{0};
    std::atomic<uint32_t> dbgSaveMaxUs_{0};
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
    uint32_t playEndBars();
    bool bounceSeqTrack(uint8_t track);
    bool freezeTrack(uint8_t track);
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
    static void worker_entry(void* arg) { static_cast<Impl*>(arg)->worker(); }
    bool request(uint8_t list);
    bool claimEdit();
    bool requestEdit(uint8_t edit);
    void waitRtOut();
    void doListProjects();
    void doListClips();
    SongOpState doOpen(const std::string& name);
    void openLast();
    SongOpState doRename();
    SongOpState doDelete();
    SongOpState doTempo(float bpm);
    SongOpState doMerge(uint8_t mask, uint32_t fromBar, uint32_t toBar);
    SongOpState doExport();
    SongOpState doImport();
    void doSave();
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
    bool openTake(SyncTake take);
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
    static std::string stretchName(const SongClip& c, uint32_t outFrames, StretchMode mode);
    bool stretchReady(const std::string& path) const;
    struct StretchJob { std::string src, dst; uint32_t offset = 0, frames = 0, outFrames = 0; bool repitch = false; };
    uint32_t nextStretch(StretchJob* job);
    bool stretchPending() { return nextStretch(nullptr) != 0; }
    void doStretch();
    bool requestWaiting() const;
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
    bool        takeRecMute_ = false;            // a REC's pass not landed yet: not heard under it
    /* What applySeqSound() last gave setSeqGain(), kit and pitch; under
     * songMutex_. */
    float       seqGain_[3] = {1.0f, 1.0f, 1.0f};
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
    uint32_t    importA_ = 0, importB_ = 0, importBar_ = 0, importLead_ = 0;   // owned by claimEdit()'s caller until editReq_
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
    std::atomic<int8_t>   seqRoute_[kTracks] = {{-1}, {-1}, {-1}, {-1}};   ///< AudioSong::seqRoute(), per track
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
    std::atomic<uint32_t> stretchCount_{0};
    std::atomic<uint8_t>  stretchProg_{0};   ///< the block being fitted, 0..100
    // Undo: steps back and forward, and the arrangement as last seen (all
    // under songMutex_); the counts for the screen.
    std::vector<Snapshot> undo_, redo_;
    Snapshot              last_;
    int64_t               lastChangeUs_ = 0;
    std::atomic<uint32_t> undoCount_{0}, redoCount_{0};
};

} // namespace crosspad

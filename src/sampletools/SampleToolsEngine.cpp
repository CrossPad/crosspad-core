// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sample tools: a take from the mics, the line-in, the pads or USB to a
// WAV on the card, a region of a WAV played back for audition, and the file
// jobs behind the Recorder and the Slicer -- listing takes, drawing a file,
// saving a loop, rendering a pad's copy, writing a sliced loop out as a kit.
//
// Threads, as in the song engine:
//   render     meters the selected input and hands it to the recording ring,
//              and plays the preview ring out. Never blocks, never the card.
//   worker     drains a take to the card, keeps the preview ring filled, and
//              runs one file job at a time -- and between a job's chunks it
//              does the first two, so a long job never starves a take or an
//              audition.
//   UI/CLI     asks, and polls.
//
// A take with the song (startRecordingWithSong) is armed first, then the
// song engine is asked to play. The render marks where each block began in
// the take (rtBlockBegin); after the mixer it asks the engine how much it has
// played, and the first block that is not nothing is the one the song
// started in -- that block's first input frame, moved on by the codecs'
// delay, is the take's frame 0. Until then the worker throws the lead-in away
// and keeps only the newest part of it in the ring.
//
// A song already playing -- PLAY on the transport bar, Arrange -- is not
// started again (that restarted it from FROM, under the player's feet): the
// take goes in at the next bar line the song reaches. The render works out
// that line from where the song is (ISongEngine::positionFrames()) and how
// far into its own block the line falls, which is all frame 0 needs.

#include "crosspad/sampletools/SampleToolsEngine.hpp"

#include "crosspad/audio/WavWriter.hpp"
#include "crosspad/dsp/TimeStretch.hpp"
#include "crosspad/instrument/InstrumentPlayState.hpp"
#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/kit/KitFolders.hpp"
#include "crosspad/kit/KitInfo.hpp"
#include "crosspad/platform/IFileSystem.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sampletools/SampleFx.hpp"
#include "crosspad/sampletools/SliceMath.hpp"
#include "crosspad/song/ISongEngine.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>

namespace crosspad {
namespace {

constexpr const char* kCrosspadDir = "/crosspad";
constexpr const char* kRecSubdir   = "/crosspad/recordings";
/* A kit the Slicer makes goes with the others it made: the browser's SLICE
 * list is that folder. */
constexpr const char* kSlicerKits = KitFolders::kSlicer;
constexpr uint32_t kRecRingFrames  = 65536;   // 256 kB, ~1.4 s of card stall
constexpr uint32_t kRecMinFrames   = 16384;
constexpr uint32_t kPrevRingFrames = 8192;    // ~170 ms ahead
constexpr uint32_t kPrevPrime      = 4096;
constexpr uint32_t kChunk          = 1024;    // 4 kB per card access
constexpr uint32_t kWinFrames      = kChunk * 2 + 4;   // a chunk an octave up, and its taps
constexpr uint32_t kMaxTakeSeconds = 600;
constexpr uint32_t kEnvHop         = 256;     // ~5 ms: fine enough to place a cut by ear
constexpr size_t   kMaxNameLen     = 32;
constexpr uint32_t kIdlePollMs     = 3;
/// Frames from the block the song goes out to the codecs in to the block the
/// ADC hands back what was heard with it: DMA out, the codecs' filters, DMA
/// in. Not measured yet, so 0: a take sits that much late, estimated 5-12 ms
/// from the 4 x 64-frame DMA each way (sample-tools.md says how to measure).
constexpr uint32_t kCodecDelayFrames = 0;
/// rtPunch(): the song ends before the bar a take would go in at.
constexpr uint32_t kNoPunchBar = UINT32_MAX;
constexpr uint32_t kSyncKeep       = 16384;   // lead-in kept while the song primes, ~0.35 s
constexpr int64_t  kSongStartMs    = 3000;    // a song not playing by then is not coming
/// Longer than one render block: the render, which may be inside take() or
/// mix() when a ring is about to go, has left by then.
constexpr uint32_t kRtLeaveMs      = 10;
/// How many directory entries the worker reads between its realtime duties.
constexpr uint32_t kDirServiceEvery = 32;
/// The overview's columns are int16 peaks scaled to -127..127.
constexpr int      kPeakScale      = 258;
/// Normalize: a file whose loudest sample is within this gain of the asked
/// peak (0.1 dB), or above it, is left as it is.
constexpr float    kNormalizeSlack = 1.0116f;
/// Normalize's progress: the peak pass is the first half, the write the rest.
constexpr uint32_t kNormalizeScanPct = 49;
constexpr uint32_t kNormalizeWritePct = 50;
/// reclaimLentRing(): how long it waits, and in what steps.
constexpr int      kLookTries      = 1000;    // x 1 ms: to read whether the ring is lent
constexpr int      kAnswerTries    = 200;     // x 10 ms: for the worker to give it back
constexpr uint32_t kAnswerPollMs   = 10;
constexpr int      kDropTries      = 1000;    // x 1 ms: to mark it lost

/// The few file operations the tools need, on the platform's file system.
namespace fsx {
IFileSystem& fs() { return getFileSystem(); }
int  openRead(const std::string& p) { return fs().openRead(p.c_str()); }
long readSome(int fd, void* dst, size_t n) { return fs().read(fd, dst, n); }
bool seek(int fd, uint32_t at) { return fs().seek(fd, at); }
void close(int fd) { fs().close(fd); }
bool exists(const std::string& p) { return fs().exists(p.c_str()); }
bool makeDir(const std::string& p) { return fs().makeDir(p.c_str()); }
/// @p p and every folder above it that is not there yet, as `mkdir -p`: a
/// file written into a new folder two deep failed with "cannot make".
bool makeDirs(const std::string& p)
{
    if (p.empty() || exists(p)) return true;
    /* A parent that will not be made may be a mount point a stat cannot see
     * ("/sdcard"): only the folder asked for has to be there in the end. */
    const size_t slash = p.find_last_of('/');
    if (slash != std::string::npos && slash > 0) makeDirs(p.substr(0, slash));
    return makeDir(p) || exists(p);
}
bool remove(const std::string& p) { return fs().remove(p.c_str()); }
bool rename(const std::string& from, const std::string& to) { return fs().rename(from.c_str(), to.c_str()); }
/// ASCII without case, as strcasecmp() in the C locale.
int  compareNoCase(const char* a, const char* b)
{
    for (;; ++a, ++b) {
        const int ca = std::tolower((unsigned char)*a);
        const int cb = std::tolower((unsigned char)*b);
        if (ca != cb || ca == 0) return ca - cb;
    }
}
} // namespace fsx

/// A kit name that is one folder FAT will take.
bool valid_name(const std::string& n)
{
    if (n.empty() || n.size() > kMaxNameLen || n == "." || n == ".." || n.back() == ' ' || n.back() == '.') return false;
    for (char c : n) if ((unsigned char)c < 32) return false;
    return n.find_first_of("/\\:*?\"<>|") == std::string::npos;
}

/// The first free "<dir>/<stem>_NNN.wav".
std::string next_free(const std::string& dir, const char* stem)
{
    constexpr int kMaxNumbered = 1000;
    /* Built as a string: a fixed buffer cut a long folder's path short, and
     * the cut name was never free -- "no free name" on any take there. */
    for (int n = 1; n < kMaxNumbered; ++n) {
        char tail[24];
        std::snprintf(tail, sizeof(tail), "_%03d.wav", n);
        std::string path = dir + "/" + stem + tail;
        if (!fsx::exists(path)) return path;
    }
    return std::string();
}

std::string base_of(const std::string& path)
{
    const size_t s = path.find_last_of('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

/// The codecs an input records from, a bit each: 0 the mics, 1 the line-in.
uint8_t codecs_of(RecInput in)
{
    switch (in) {
        case RecInput::Mic:     return 1u << 0;
        case RecInput::Line:    return 1u << 1;
        case RecInput::MicLine: return (1u << 0) | (1u << 1);
        default:                return 0;
    }
}

/// Sixteen hues round the wheel, one per slice, so pad and slice match.
uint32_t slice_color(int i)
{
    const float h = (float)(i % 16) / 16.0f * 6.0f;
    const int   k = (int)h;
    const float f = h - (float)k;
    float r = 0, g = 0, b = 0;
    switch (k) {
        case 0: r = 1; g = f; break;
        case 1: r = 1 - f; g = 1; break;
        case 2: g = 1; b = f; break;
        case 3: g = 1 - f; b = 1; break;
        case 4: r = f; b = 1; break;
        default: r = 1; b = 1 - f; break;
    }
    return ((uint32_t)(r * 255) << 16) | ((uint32_t)(g * 255) << 8) | (uint32_t)(b * 255);
}

bool is_wav(const char* n)
{
    const size_t len = std::strlen(n);
    return len > 4 && fsx::compareNoCase(n + len - 4, ".wav") == 0;
}

enum class Prev : uint8_t { Idle, Priming, Running, Ended };
enum class JobKind : uint8_t { None, List, Folder, Overview, SaveRegion, Export, Rendered, Normalize };

/// Stereo int16 frames, one writer and one reader, counters that only grow
/// (so used() is right across the uint32 wrap). In 16 kB segments rather than
/// one block: on the board the largest free PSRAM block is often well under
/// 100 kB, and a ring that needed one contiguous 128 kB piece failed ("not
/// enough memory") with plenty free in total. The platform says where each
/// segment comes from; a ring may be lent memory the platform wants back.
struct FrameRing {
    ISampleToolsPlatform* platform = nullptr;
    static constexpr uint32_t kSegFrames = 4096;       // 16 kB
    static constexpr uint32_t kMaxSegs   = 16;
    int16_t* seg[kMaxSegs] = {};
    uint32_t segs = 0;
    uint32_t cap = 0;
    /// Which segments were lent (bit per segment): such a ring has to go
    /// before the platform takes them back. The owner's thread only (alloc,
    /// release, forget).
    uint16_t lent = 0;
    bool borrowed() const { return lent != 0; }
    std::atomic<uint32_t> head{0}, tail{0};

    static constexpr size_t kSegBytes = kSegFrames * 4;

    /// As many segments as memory gives, up to @p frames, at least @p
    /// minFrames; lent ones too with @p mayBorrow.
    bool alloc(uint32_t frames, uint32_t minFrames, bool mayBorrow) {
        release();
        const uint32_t want = std::min(kMaxSegs, (frames + kSegFrames - 1) / kSegFrames);
        const uint32_t need = (minFrames + kSegFrames - 1) / kSegFrames;
        while (segs < want) {
            bool wasLent = false;
            void* p = platform->allocRingSegment(kSegBytes, segs < need ? need - segs : 1, mayBorrow, wasLent);
            if (!p) break;
            if (wasLent) lent = static_cast<uint16_t>(lent | (1u << segs));
            seg[segs++] = static_cast<int16_t*>(p);
        }
        cap = segs * kSegFrames;
        if (cap < minFrames) { release(); return false; }
        return true;
    }
    void release() {
        for (uint32_t i = 0; i < segs; ++i) platform->freeRingSegment(seg[i]);
        clear();
    }
    /// The lent segments were taken back under the ring: they are dropped
    /// without a free (memory the platform no longer counts as given), the
    /// others are freed as usual.
    void forget() {
        for (uint32_t i = 0; i < segs; ++i) {
            if (!(lent & (1u << i))) platform->freeRingSegment(seg[i]);
        }
        clear();
    }
    void clear() {
        std::fill(std::begin(seg), std::end(seg), nullptr);
        segs = 0; cap = 0;
        lent = 0;
        head.store(0); tail.store(0);
    }
    uint32_t used()  const { return head.load(std::memory_order_acquire) - tail.load(std::memory_order_acquire); }
    uint32_t space() const { return cap - used(); }

    /// Copy @p n frames between @p ext and the ring from frame @p at on,
    /// segment by segment.
    void copy(uint32_t at, int16_t* ext, uint32_t n, bool toRing) {
        while (n) {
            const uint32_t pos = at % cap;
            int16_t* p = seg[pos / kSegFrames] + (pos % kSegFrames) * 2;
            const uint32_t run = std::min(n, kSegFrames - pos % kSegFrames);
            if (toRing) std::memcpy(p, ext, run * 4);
            else        std::memcpy(ext, p, run * 4);
            ext += run * 2;
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

/// A WAV being read: where its samples start and how it is laid out. 16-bit
/// PCM, mono or stereo; mono is played to both sides. A plain descriptor, not
/// a FILE: reads are whole chunks, and stdio's 4 kB buffer per open file is
/// memory a board does not have to spare.
struct ClipReader {
    ClipReader() = default;
    /* The descriptor is given back whatever happens: the card has a handful
     * and one held open is one a pad's sample cannot have. Not copyable, so
     * a copy cannot close another's. */
    ~ClipReader() { close(); }
    ClipReader(const ClipReader&) = delete;
    ClipReader& operator=(const ClipReader&) = delete;

    int         fd = -1;
    std::string path;
    uint32_t    dataOffset = 0;
    uint32_t    frames = 0;
    uint16_t    channels = 2;
    uint32_t    rate = 0;              // the file's own, from its fmt chunk
    uint32_t    cursor = UINT32_MAX;   // frame the file position is at

    void close() { if (fd >= 0) fsx::close(fd); fd = -1; path.clear(); cursor = UINT32_MAX; }

    bool readAll(void* dst, size_t n) { return fsx::readSome(fd, dst, n) == (long)n; }

    /// @p ready: the platform says the card can be opened now.
    bool open(const std::string& p, bool ready) {
        close();
        if (!ready) return false;
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
                rate = fm[4] | fm[5] << 8 | fm[6] << 16 | (uint32_t)fm[7] << 24;
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

/// @p n stereo frames that start @p rel frames into a slice @p len long,
/// shaped as the slice plays (SliceMath::sliceGain).
void shape(int16_t* s, uint32_t n, uint32_t rel, uint32_t len, uint32_t attack)
{
    for (uint32_t k = 0; k < n; ++k) {
        const float g = SliceMath::sliceGain(rel + k, len, attack);
        if (g < 1.0f) {
            s[k * 2]     = (int16_t)((float)s[k * 2] * g);
            s[k * 2 + 1] = (int16_t)((float)s[k * 2 + 1] * g);
        }
    }
}

} // namespace

struct SampleToolsEngine::Impl {
    explicit Impl(ISampleToolsPlatform& platform) : p(platform) {
        rRing_.platform = &p;
        pRing_.platform = &p;
    }

    /// The worker is told to quit and joined. Only a platform that destroys
    /// the engine (PC, tests) gets here.
    void shutdown() {
        if (!workerUp_.load()) return;
        quit_.store(true);
        p.wake();
        p.joinWorker();
        rRing_.release();
        pRing_.release();
    }

    /// printf into the platform's log.
    void logf(bool warning, const char* fmt, ...) {
        char msg[192];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        p.log(warning, msg);
    }

    std::string recDir() { return p.cardRoot() + kRecSubdir; }
    /// The recordings folder, made when it is not there yet.
    bool makeRecDir() { return fsx::makeDir(p.cardRoot() + kCrosspadDir) && fsx::makeDir(recDir()); }

    // ── IAudioNode: the preview, played into the mixer (render) ───────────
    void onPrepare(uint32_t maxFrames, uint32_t sampleRate) {
        sampleRate_ = sampleRate ? sampleRate : 44100;
        scratch_.assign(maxFrames * 2, 0);
        capture_.assign(maxFrames * 2, 0);
        held_.assign(maxFrames * 2, 0);
    }
    void onSampleRateChange(uint32_t sr) { sampleRate_ = sr ? sr : 44100; }

    void mix(float* out, uint32_t frames) {
        p.mixAudition(out, frames);
        if (prev_.load(std::memory_order_acquire) != Prev::Running) return;
        if (frames * 2 > scratch_.size()) return;
        /* Silence when the card falls behind, and the position does not move
         * for it: what previewFrame() says is what was heard, which is what a
         * cut made by ear has to land on. */
        const uint32_t got = pRing_.pop(scratch_.data(), frames);
        const int16_t* s = scratch_.data();
        for (uint32_t i = 0; i < got * 2; ++i) out[i] += (float)s[i] * (1.0f / 32768.0f);
        const uint32_t done = pDone_.load(std::memory_order_relaxed) + got;
        pDone_.store(done, std::memory_order_relaxed);
        if (!pLoop_ && done >= pOut_) prev_.store(Prev::Ended, std::memory_order_release);
    }

    // ── Render taps ───────────────────────────────────────────────────────
    bool wantsCodec(int codec) const {
        return listening_.load(std::memory_order_relaxed) && codec >= 0 && codec < 2 &&
               (codecs_of(input_.load(std::memory_order_relaxed)) & (1u << codec)) != 0;
    }
    void rtInput(int codec, const int16_t* block, uint32_t frames) {
        if (!wantsCodec(codec)) return;
        if (input_.load(std::memory_order_relaxed) != RecInput::MicLine) {
            take(block, frames);
            return;
        }
        /* Both codecs into one take: the mics' block is held until the
         * line-in's of the same render block comes, and the two are summed. */
        if (frames * 2 > held_.size()) return;
        if (codec == 0) {
            std::memcpy(held_.data(), block, frames * 2 * sizeof(int16_t));
            heldFrames_ = frames;
            return;
        }
        if (heldFrames_ == frames) {
            int16_t* d = held_.data();
            for (uint32_t i = 0; i < frames * 2; ++i) {
                const int32_t v = (int32_t)d[i] + block[i];
                d[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            }
            heldFrames_ = 0;
            take(d, frames);
            return;
        }
        heldFrames_ = 0;
        take(block, frames);       // the mics were not read this block
    }
    void rtBlockBegin() {
        /* The mics of the last block, the line-in never read: on their own
         * rather than not at all, or the take would fall behind the song. */
        if (heldFrames_) {
            const uint32_t n = heldFrames_;
            heldFrames_ = 0;
            take(held_.data(), n);
        }
        blockBegin_ = recFrames_.load(std::memory_order_relaxed);
    }

    void rtUsb(const int16_t* block, uint32_t frames) {
        if (listening_.load(std::memory_order_relaxed) &&
            input_.load(std::memory_order_relaxed) == RecInput::Usb) take(block, frames);
    }

    bool wantsBus() const {
        if (syncWait_.load(std::memory_order_acquire)) return true;
        const RecInput in = input_.load(std::memory_order_relaxed);
        return listening_.load(std::memory_order_relaxed) && (in == RecInput::Pads || in == RecInput::Pitch);
    }
    void rtBus(const float* bus, uint32_t frames) {
        if (syncWait_.load(std::memory_order_acquire)) rtSync(frames);
        // Pitch is the same bus: a borrowed pitched bank renders into it where
        // a kit's streamer would, so only the engine behind it differs.
        const RecInput in = input_.load(std::memory_order_relaxed);
        if (!listening_.load(std::memory_order_relaxed) ||
            (in != RecInput::Pads && in != RecInput::Pitch)) return;
        if (frames * 2 > capture_.size()) return;
        int16_t* d = capture_.data();
        for (uint32_t i = 0; i < frames * 2; ++i) {
            const float v = bus[i] * 32767.0f;
            d[i] = (int16_t)(v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
        }
        take(d, frames);
    }

    void setCaptureTap(CaptureTap fn, void* ctx) {
        captureTapCtx_ = ctx;
        captureTap_.store(fn, std::memory_order_release);
    }

    // ── Input and recording ───────────────────────────────────────────────
    bool selectInput(RecInput in) {
        std::lock_guard<std::mutex> l(inputMutex_);
        if (takeOpen()) return false;   // not mid-take
        releaseAdc();
        input_.store(in);
        const uint8_t codecs = codecs_of(in);
        for (uint8_t c = 0; c < 2; ++c)
            if ((codecs & (1u << c)) && p.claimCodec(c, true)) claimed_ |= (uint8_t)(1u << c);
        listening_.store(true);
        return true;
    }
    bool inputAvailable(RecInput in) const {
        /* Pitch needs an engine to borrow and an instrument to borrow it for;
         * without either there is nothing for the pads to sound. */
        if (in == RecInput::Pitch) {
            return getPlatformServices().instrumentHost != nullptr &&
                   !instrumentPlayState().instrumentPath.empty();
        }
        return p.inputAvailable(in);
    }

    void peaks(float& left, float& right) {
        left  = (float)pkL_.exchange(0) / 32768.0f;
        right = (float)pkR_.exchange(0) / 32768.0f;
    }

    bool requestTake(bool withSong, uint32_t fromBar, uint8_t countInBeats) {
        if (!listening_.load() || takeOpen() || !ensureWorker()) return false;
        if (!inputAvailable(input_.load())) return false;      // USB outside the USB audio profile
        {
            std::lock_guard<std::mutex> l(statusMutex_);
            rec_ = RecStatus{};
            rec_.recording = true;
            rec_.sampleRate = sampleRate_;
            rec_.withSong = withSong;
            rec_.fromBar = fromBar;
        }
        recWithSong_ = withSong;      // read by the worker after recStartReq_
        recFromBar_ = fromBar;
        recCountIn_ = countInBeats;
        /* Waiting from the request on, not from when the worker arms it: the
         * screen read "playing" for a moment in between. */
        skip_.store(withSong ? UINT32_MAX : 0);
        recStopReq_.store(false);     // a stop from before this take is not for it
        recFrames_.store(0);
        recStartReq_.store(true);
        p.wake();
        return true;
    }

    void stopRecording() {
        recStopReq_.store(true);
        if (workerUp_.load()) p.wake();
    }

    RecStatus recStatus() {
        std::lock_guard<std::mutex> l(statusMutex_);
        RecStatus s = rec_;
        if (s.recording) {
            const uint32_t f = recFrames_.load(), skip = skip_.load();
            s.waiting = s.withSong && skip == UINT32_MAX;
            s.frames = !s.withSong ? f : (s.waiting || f < skip ? 0 : f - skip);
        }
        return s;
    }

    void releaseInput() {
        stopRecording();
        stopPreview();
        std::lock_guard<std::mutex> l(inputMutex_);
        listening_.store(false);
        releaseAdc();
    }

    // ── Files ─────────────────────────────────────────────────────────────
    bool requestFolder(const std::string& dir) {
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            if (job_.load() == ToolJob::Busy) return false;
            pFile_ = dir;
        }
        return request(JobKind::Folder);
    }
    CardFolder folder() {
        std::lock_guard<std::mutex> l(resultMutex_);
        return folder_;
    }
    std::vector<std::string> recordings() {
        std::lock_guard<std::mutex> l(resultMutex_);
        return recordings_;
    }

    bool requestOverview(const std::string& file, uint32_t start, uint32_t end, uint16_t columns) {
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            if (job_.load() == ToolJob::Busy) return false;
            pFile_ = file; pA_ = start; pB_ = end; pCols_ = std::max<uint16_t>(1, columns);
        }
        return request(JobKind::Overview);
    }
    WaveOverview overview() {
        std::lock_guard<std::mutex> l(resultMutex_);
        return overview_;
    }

    bool requestSaveRegion(const std::string& file, uint32_t start, uint32_t end) {
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            if (job_.load() == ToolJob::Busy) return false;
            pFile_ = file; pA_ = start; pB_ = end;
        }
        return request(JobKind::SaveRegion);
    }

    bool requestRendered(const std::string& file, const std::string& out, const SampleFx& fx) {
        if (file.empty() || out.empty() || !fx.any()) return false;
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            if (job_.load() == ToolJob::Busy) return false;
            pFile_ = file; pDst_ = out; pFx_ = fx;
        }
        return request(JobKind::Rendered);
    }

    bool requestNormalized(const std::string& file, const std::string& out, float peak) {
        if (file.empty() || out.empty() || !(peak > 0.0f && peak <= 1.0f)) return false;
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            if (job_.load() == ToolJob::Busy) return false;
            pFile_ = file; pDst_ = out; pPeak_ = peak;
        }
        return request(JobKind::Normalize);
    }

    bool requestExportKit(const SliceKit& kit) {
        if (!valid_name(kit.name) || kit.starts.empty() || kit.starts.size() > SliceMath::kMaxSlices) return false;
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            if (job_.load() == ToolJob::Busy) return false;
            pKit_ = kit;
        }
        return request(JobKind::Export);
    }

    std::string jobResult() {
        std::lock_guard<std::mutex> l(resultMutex_);
        return result_;
    }

    // ── Preview ───────────────────────────────────────────────────────────
    bool previewAs(const std::string& file, uint32_t start, uint32_t end, bool loop, const PreviewSound& sound) {
        if (file.empty() || !ensureWorker()) return false;
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            prevFile_ = file; prevA_ = start; prevB_ = end; prevLoop_ = loop;
            prevAttack_ = sound.slice ? SliceMath::attackFrames(sound.attackMs, sampleRate_) : 0;
            prevRatio_ = SliceMath::pitchRatio(sound.semitones);
            prevFx_ = SampleFx{sound.semitones, sound.hpHz, sound.lpHz, sound.reverse};
        }
        prevStopReq_.store(false);
        prevGen_.fetch_add(1);
        p.wake();
        return true;
    }
    void stopPreview() {
        prevDone_.store(prevGen_.load());          // a request not started yet is dropped
        if (prev_.load() == Prev::Idle) return;
        prevStopReq_.store(true);
        if (workerUp_.load()) p.wake();
    }
    /// From the request on, not from the first sound: between the two the
    /// worker is still opening the file, and a caller that reads "not
    /// playing" there takes the audition for over (the Slicer's FREE did).
    bool previewing() const {
        const Prev pr = prev_.load();
        return pr == Prev::Priming || pr == Prev::Running || prevGen_.load() != prevDone_.load();
    }
    /// In the file's frames: a played frame is pRatio_ of them, counted from
    /// the region's end when it plays backwards.
    uint32_t previewFrame() const {
        if (prev_.load() == Prev::Idle || !pOut_) return pStart_;
        const uint32_t done = pDone_.load(std::memory_order_relaxed);
        const uint32_t played = pLoop_ ? done % pOut_ : std::min(done, pOut_);
        const uint32_t into = std::min(pLen_, (uint32_t)((double)played * pRatio_));
        return pReverse_ ? pStart_ + (pLen_ - std::max<uint32_t>(1, into)) : pStart_ + into;
    }

    // ── Handoff ───────────────────────────────────────────────────────────
    void setHandoff(const std::string& file, uint32_t start, uint32_t end) {
        std::lock_guard<std::mutex> l(paramMutex_);
        hFile_ = file; hA_ = start; hB_ = end;
    }
    bool takeHandoff(std::string& file, uint32_t& start, uint32_t& end) {
        std::lock_guard<std::mutex> l(paramMutex_);
        if (hFile_.empty()) return false;
        file = hFile_; start = hA_; end = hB_;
        hFile_.clear();
        return true;
    }

    /// Render, after the mixer: the song's first block, while a take waits for it.
    void rtSync(uint32_t frames) {
        ISongEngine* se = getPlatformServices().songEngine;
        if (punch_.load(std::memory_order_relaxed)) { rtPunch(se, frames); return; }
        const uint32_t played = se ? se->framesPlayed() : 0;
        if (!played) { syncSeenIdle_.store(true, std::memory_order_relaxed); return; }
        if (!syncSeenIdle_.load(std::memory_order_relaxed)) return;   // the playback from before this take
        /* It sounded first in this block, or `played - frames` earlier: its
         * first frame came in with that block's first input frame. */
        const uint32_t end = blockBegin_ + frames;
        syncFrame_.store(played < end ? end - played : 0, std::memory_order_relaxed);
        syncWait_.store(false, std::memory_order_release);
    }

    /// Render: a take into the song playing now. The block just rendered
    /// began on song frame `start`; frame 0 is the first bar line from there,
    /// that many frames into this block's input -- or past the loop's end,
    /// where the song wraps to.
    void rtPunch(ISongEngine* se, uint32_t frames) {
        const uint32_t spb = punchSpb_.load(std::memory_order_relaxed);
        if (!se || !spb) return;
        const uint32_t after = se->positionFrames();
        const uint32_t start = after >= frames ? after - frames : 0;
        uint32_t bar = (start + spb - 1) / spb;
        uint32_t line = bar * spb;
        const uint32_t lim = punchLim_.load(std::memory_order_relaxed);
        if (lim && line >= lim) {
            if (!punchLoop_.load(std::memory_order_relaxed)) {
                punchBar_.store(kNoPunchBar, std::memory_order_relaxed);   // it ends first
                syncWait_.store(false, std::memory_order_release);
                return;
            }
            line = lim;
            bar = punchWrap_.load(std::memory_order_relaxed);
        }
        punchBar_.store(bar, std::memory_order_relaxed);
        syncFrame_.store(blockBegin_ + (line - start), std::memory_order_relaxed);
        syncWait_.store(false, std::memory_order_release);
    }

    /// Meter, and into the take when one is being written.
    void take(const int16_t* b, uint32_t n) {
        int32_t pl = 0, pr = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const int32_t l = b[i * 2] < 0 ? -(int32_t)b[i * 2] : b[i * 2];
            const int32_t r = b[i * 2 + 1] < 0 ? -(int32_t)b[i * 2 + 1] : b[i * 2 + 1];
            if (l > pl) pl = l;
            if (r > pr) pr = r;
        }
        if (pl > pkL_.load(std::memory_order_relaxed)) pkL_.store(pl, std::memory_order_relaxed);
        if (pr > pkR_.load(std::memory_order_relaxed)) pkR_.store(pr, std::memory_order_relaxed);
        /* Whoever else is listening to the selected input gets the same block,
         * take or no take: this is the one place every input funnels through. */
        if (CaptureTap tap = captureTap_.load(std::memory_order_relaxed)) {
            tap(captureTapCtx_, b, n);
        }
        if (!recArmed_.load(std::memory_order_acquire)) return;
        if (rRing_.push(b, n) < n) recOverflow_.store(true, std::memory_order_relaxed);
        recFrames_.fetch_add(n, std::memory_order_relaxed);
    }

    void releaseAdc() {
        for (uint8_t c = 0; c < 2; ++c)
            if (claimed_ & (1u << c)) p.claimCodec(c, false);
        claimed_ = 0;
    }

    bool request(JobKind kind) {
        if (!ensureWorker()) return false;
        ToolJob expected = job_.load();
        if (expected == ToolJob::Busy) return false;
        if (!job_.compare_exchange_strong(expected, ToolJob::Busy)) return false;
        progress_.store(0);
        jobKind_.store(kind);
        p.wake();
        return true;
    }

    /// The worker and its card buffers, on first use rather than at boot.
    bool ensureWorker() {
        if (workerUp_.load()) return true;
        std::lock_guard<std::mutex> l(workerMutex_);
        if (workerUp_.load()) return true;
        if (!io_) io_ = static_cast<int16_t*>(p.allocScratch(kChunk * 4));
        if (!win_) win_ = static_cast<int16_t*>(p.allocScratch(kWinFrames * 4));
        if (!io_ || !win_) return false;
        workerUp_.store(p.startWorker(worker_entry, this));
        return workerUp_.load();
    }

    static void worker_entry(void* arg) { static_cast<Impl*>(arg)->worker(); }

    void worker() {
        while (!quit_.load()) {
            const bool busy = recArmed_.load() || rBorrowed_.load() || takeCut_.load() || prev_.load() != Prev::Idle;
            p.waitWake(busy ? kIdlePollMs : UINT32_MAX);
            if (quit_.load()) break;
            service();
            const JobKind k = jobKind_.exchange(JobKind::None);
            if (k != JobKind::None) runJob(k);
        }
    }

    /// The realtime duties, between everything else.
    void service() {
        /* The platform wants its lent memory back (reclaimLentRing()): a
         * preview ring lent from it goes now; one from the general heap plays
         * on. */
        if (reclaimReq_.load()) {
            if (pBorrowed_.load()) endPreview();
            /* A take on lent memory ends here, written out: the kit is
             * changing under the Recorder, and what was played is kept. */
            if (rBorrowed_.load()) drainTake(true);
            reclaimReq_.store(false);
        }
        if (recStartReq_.exchange(false)) beginTake();
        /* A lent ring the reclaim took (takeCut_, or rBorrowed_ with the render
         * already stopped) still ends its take here: the file is closed and kept. */
        if (recArmed_.load() || rBorrowed_.load() || takeCut_.load()) drainTake(false);
        if (prevStopReq_.exchange(false)) endPreview();
        const uint32_t gen = prevGen_.load();
        if (gen != prevDone_.load()) {
            beginPreview();                         // reads the newest request's parameters
            prevDone_.store(gen);
        }
        const Prev pr = prev_.load();
        if (pr == Prev::Ended) endPreview();
        else if (pr != Prev::Idle) fillPreview();
    }

    /// Between a job's chunks: the card to whoever must read it in time,
    /// then the take and the preview.
    void giveWay() {
        p.yieldIo();
        service();
    }

    // ── Take ──────────────────────────────────────────────────────────────
    void failTake(const char* why) {
        endSync();
        logf(true, "take failed: %s", why);
        std::lock_guard<std::mutex> l(statusMutex_);
        rec_.recording = false;
        rec_.error = why;
    }

    void beginTake() {
        /* One writer: a take a reclaim cut is closed before another opens
         * (takeOpen() refuses the request; this is the second line). */
        if (rBorrowed_.load() || takeCut_.load()) drainTake(true);
        if (!p.storageReady() || !makeRecDir()) { failTake("no card"); return; }
        const std::string path = next_free(recDir(), "rec");
        if (path.empty()) { failTake("no free name"); return; }
        if (!writer_.open(path.c_str(), sampleRate_, 2)) { failTake("card write failed"); return; }
        /* Lent memory first: with a kit loaded the general heap keeps too
         * little above the UI's reserve for any ring. A reclaim (another kit)
         * then ends the take through reclaimLentRing(). */
        bool ok = false;
        if (ringEnter() || ringEnter()) {
            ok = rRing_.alloc(kRecRingFrames, kRecMinFrames, true);
            rBorrowed_.store(ok && rRing_.borrowed());
            ringLeave();
        }
        if (!ok) {
            writer_.close();
            fsx::remove(path);
            failTake("not enough memory");
            return;
        }
        {
            std::lock_guard<std::mutex> l(statusMutex_);
            rec_.file = path;
        }
        recFrames_.store(0);
        recOverflow_.store(false);
        syncTake_ = recWithSong_;
        dropped_ = 0;
        skip_.store(syncTake_ ? UINT32_MAX : 0);
        ISongEngine* se = getPlatformServices().songEngine;
        /* Playing already, and not counting in: the take goes in at its next
         * bar line and the song is left alone. */
        bool punch = false;
        if (syncTake_ && se && se->playing() && !se->countInBeatsLeft()) {
            std::lock_guard<std::recursive_mutex> l(se->songMutex());
            const AudioSong& song = se->song();
            const uint32_t spb = se->framesPerBar();
            const bool region = song.loop && song.hasRegion();
            punchSpb_.store(spb);
            punchLim_.store((region ? song.loopEnd : se->playEndBars()) * spb);
            punchWrap_.store(region ? song.loopStart : 0);
            punchLoop_.store(song.loop);
            punch = spb != 0;
        }
        punch_.store(punch);
        if (syncTake_) {
            syncSeenIdle_.store(false);
            syncWait_.store(true, std::memory_order_release);
            armedUs_ = p.nowUs();
        }
        recArmed_.store(true, std::memory_order_release);
        /* Armed first: the take is listening before the song can sound. Only
         * a take with the song starts it -- SYNC off leaves the song alone, and
         * so does a take into a song already playing. */
        const bool played = syncTake_ && se &&
                            (punch || (recCountIn_ ? se->playCountedIn(recFromBar_, SyncTake::None, recCountIn_)
                                                   : se->play(recFromBar_)));
        if (syncTake_ && !played) {
            recArmed_.store(false, std::memory_order_release);
            p.sleepMs(kRtLeaveMs);    // the render may be inside take()
            releaseTakeRing();
            writer_.close();
            fsx::remove(path);
            failTake("the song cannot play: no blocks, or a bounce running");
            return;
        }
        logf(false, "recording %s%s", path.c_str(), syncTake_ ? " with the song" : "");
    }

    /// The song stops with a take that started it.
    void endSync() {
        if (!syncTake_) return;
        syncTake_ = false;
        syncWait_.store(false, std::memory_order_release);
        punch_.store(false);
        if (ISongEngine* se = getPlatformServices().songEngine) se->stop();
    }

    /// A take is under way or not yet closed -- one a reclaim cut is still
    /// being written out with the render already stopped.
    bool takeOpen() const {
        return recArmed_.load() || recStartReq_.load() || rBorrowed_.load() || takeCut_.load();
    }

    /// Up to @p n frames of the take into io_, the ring held only for the
    /// copy; 0 once a reclaim took a lent ring (the take is cut there).
    uint32_t popTake(uint32_t n) {
        if (!ringEnter()) return 0;
        const uint32_t got = rRing_.pop(io_, n);
        ringLeave();
        return got;
    }

    void releaseTakeRing() {
        if (!ringEnter()) return;             // lost to a reclaim: already forgotten
        rRing_.release();
        rBorrowed_.store(false);
        ringLeave();
    }

    void drop(uint32_t n) {
        while (n) {
            const uint32_t got = popTake(std::min(kChunk, n));
            if (!got) break;
            dropped_ += got;
            n -= got;
        }
    }

    /// A take with the song: what came in before its frame 0 is thrown away.
    /// Until the song sounds, the newest kSyncKeep frames stay in the ring --
    /// frame 0 is among them once it does. Returns why the take cannot go
    /// on, or nullptr.
    const char* leadIn(bool stopping) {
        uint32_t skip = skip_.load();
        if (skip == UINT32_MAX) {
            if (syncWait_.load(std::memory_order_acquire)) {
                if (stopping) return "stopped before the song started";
                ISongEngine* se = getPlatformServices().songEngine;
                /* A count-in is the song being on its way: timed from its end. */
                if (se && se->countInBeatsLeft()) armedUs_ = p.nowUs();
                const int64_t waited = (p.nowUs() - armedUs_) / 1000;
                if ((waited > kSongStartMs && !(se && se->playing())) || waited > 4 * kSongStartMs) {
                    return "the song did not start";
                }
                const uint32_t used = rRing_.used();
                drop(used > kSyncKeep ? used - kSyncKeep : 0);
                return nullptr;
            }
            if (punch_.load()) {
                /* Into the playing song: the bar it went in at is the take's
                 * bar, whatever FROM said. */
                const uint32_t bar = punchBar_.load();
                if (bar == kNoPunchBar) return "the song ends before its next bar";
                std::lock_guard<std::mutex> l(statusMutex_);
                rec_.fromBar = bar;
            }
            const uint32_t delay = codecs_of(input_.load()) != 0 ? kCodecDelayFrames : 0;
            skip = syncFrame_.load() + delay;
            if (skip < dropped_) return "the song's first frame was lost";
            skip_.store(skip);
        }
        if (dropped_ < skip) drop(skip - dropped_);
        return nullptr;
    }

    /// Ring to card. @p finishing: the take is over, write what is left.
    void drainTake(bool finishing) {
        const uint32_t maxFrames = kMaxTakeSeconds * sampleRate_;
        bool stop = recStopReq_.exchange(false) || finishing || recFrames_.load() >= maxFrames;
        const bool overflow = recOverflow_.load();
        const char* syncError = syncTake_ ? leadIn(stop || overflow) : nullptr;
        if (stop || overflow || syncError) {
            recArmed_.store(false, std::memory_order_release);
            p.sleepMs(kRtLeaveMs);    // the render may be inside take()
        }
        /* With the song, nothing is written before frame 0. */
        const bool beforeZero = syncTake_ && (skip_.load() == UINT32_MAX || dropped_ < skip_.load());
        for (;;) {
            if (syncError || beforeZero) break;
            const uint32_t n = popTake(kChunk);
            if (!n) break;
            if (!writer_.write(io_, n)) { overflowWrite_ = true; break; }
            if (!stop && !overflow) break;     // one chunk a pass while the take runs
        }
        if (!stop && !overflow && !overflowWrite_ && !syncError && !takeCut_.load()) return;
        releaseTakeRing();
        takeCut_.store(false);
        const uint32_t frames = writer_.frames();
        const bool closed = writer_.close();
        std::string file;
        {
            std::lock_guard<std::mutex> l(statusMutex_);
            file = rec_.file;
        }
        if (!syncError && syncTake_ && !frames) syncError = "stopped before the song started";
        if (syncError || overflow || overflowWrite_ || !closed) {
            overflowWrite_ = false;
            fsx::remove(file);
            failTake(syncError ? syncError : (overflow ? "card too slow, audio lost" : "card write failed"));
            return;
        }
        endSync();
        /* Nothing came in: no file for it, or the recordings fill with empty
         * takes every browser then lists. */
        if (!frames) fsx::remove(file);
        logf(false, "took %s (%u frames)", file.c_str(), (unsigned)frames);
        std::lock_guard<std::mutex> l(statusMutex_);
        rec_.recording = false;
        rec_.frames = frames;
    }

    // ── Preview ───────────────────────────────────────────────────────────
    void beginPreview() {
        endPreview();
        std::string file;
        uint32_t a, b, attack;
        double ratio;
        bool loop;
        SampleFx fx;
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            file = prevFile_; a = prevA_; b = prevB_; loop = prevLoop_; attack = prevAttack_; ratio = prevRatio_;
            fx = prevFx_;
        }
        if (pReader_.path != file && !pReader_.open(file, p.storageReady())) return;
        if (!b || b > pReader_.frames) b = pReader_.frames;
        if (a >= b) return;
        bool got = false;
        /* Twice: a ring lost to a reclaim is forgotten by the first. */
        if (ringEnter() || ringEnter()) {
            got = pRing_.alloc(kPrevRingFrames, kPrevPrime, true);
            pBorrowed_.store(pRing_.borrowed());
            ringLeave();
        }
        if (!got) return;
        pStart_ = a;
        pLen_ = b - a;
        pLoop_ = loop;
        pAttack_ = attack;
        pRatio_ = ratio;
        pOut_ = SliceMath::pitchedFrames(pLen_, ratio);
        pReverse_ = fx.reverse;
        /* At the file's own rate, as the rendered copy is filtered. */
        pFilters_.set(fx, pReader_.rate ? pReader_.rate : sampleRate_);
        pWrite_ = 0;
        pDone_.store(0);
        prev_.store(Prev::Priming, std::memory_order_release);
        fillPreview();
    }

    void fillPreview() {
        while (pRing_.space() >= kChunk) {
            if (!pLoop_ && pWrite_ >= pOut_) break;
            const uint32_t at = pWrite_ % pOut_;
            const uint32_t n = std::min(kChunk, pOut_ - at);
            readPlayed(pReader_, pStart_, pLen_, pRatio_, at, io_, n, pReverse_);
            pFilters_.process(io_, n);
            if (pAttack_) shape(io_, n, at, pOut_, pAttack_);
            if (!ringEnter()) return;             // reclaimed under us: the preview is over
            pRing_.push(io_, n);
            ringLeave();
            pWrite_ += n;
        }
        if (prev_.load() == Prev::Priming &&
            (pRing_.used() >= kPrevPrime || (!pLoop_ && pWrite_ >= pOut_))) {
            prev_.store(Prev::Running, std::memory_order_release);
        }
    }

    void endPreview() {
        if (prev_.load() == Prev::Idle) return;
        prev_.store(Prev::Idle, std::memory_order_release);
        p.sleepMs(kRtLeaveMs);        // the render may be inside mix()
        if (ringEnter()) {
            pRing_.release();
            pBorrowed_.store(false);
            ringLeave();
        }
    }

    /* Who has the lent rings' memory -- the preview's and the take's: 0
     * nobody, 1 this worker (a push, a pop, an alloc, a release --
     * microseconds, never across a card access), 2 lost: reclaimLentRing()
     * gave up waiting for the worker and the platform took its memory back
     * under a lent ring. The worker then forgets them rather than writes or
     * frees them. 3: reclaimLentRing() is reading whether a ring is lent -- a
     * moment; the worker waits it out. False: a ring was lost, and is now
     * forgotten and empty. */
    static constexpr uint8_t kRingFree = 0, kRingWorker = 1, kRingLost = 2, kRingLooked = 3;
    bool ringEnter() {
        uint8_t e = kRingFree;
        while (!ringUse_.compare_exchange_strong(e, kRingWorker)) {
            if (e != kRingLooked) break;
            p.sleepMs(0);
            e = kRingFree;
        }
        if (e == kRingFree) return true;
        pRing_.forget();
        pBorrowed_.store(false);
        prev_.store(Prev::Idle, std::memory_order_release);
        if (rBorrowed_.load()) {
            /* The take stops where its ring went; what reached the card stays. */
            rRing_.forget();
            rBorrowed_.store(false);
            recArmed_.store(false, std::memory_order_release);
            takeCut_.store(true);
        }
        ringUse_.store(kRingFree);
        logf(true, "a lent ring was reclaimed under it");
        return false;
    }
    void ringLeave() { ringUse_.store(kRingFree); }

    void reclaimLentRing() {
        if (!workerUp_.load()) return;
        /* Whether the ring is lent is read holding the ring word, not before:
         * the worker sets pBorrowed_ inside its own hold, right after the
         * alloc, so a read outside one could see "not lent" a moment before a
         * lent ring existed -- and the memory was then taken back under it.
         * The worker never holds the word across a card access; 1 s bound. */
        bool looked = false;
        for (int i = 0; i < kLookTries && !looked; ++i) {
            uint8_t e = kRingFree;
            looked = ringUse_.compare_exchange_strong(e, kRingLooked);
            if (!looked && e == kRingLost) return;   /* lost to an earlier reclaim: never freed into this one */
            if (!looked) p.sleepMs(1);
        }
        if (!looked) {
            logf(true, "reclaim: could not look at the preview ring for 1 s");
            return;
        }
        const bool lent = pBorrowed_.load() || rBorrowed_.load();
        ringUse_.store(kRingFree);
        if (!lent) return;
        reclaimReq_.store(true);
        p.wake();
        for (int i = 0; i < kAnswerTries && reclaimReq_.load(); ++i) p.sleepMs(kAnswerPollMs);
        if (!reclaimReq_.load() || !(pBorrowed_.load() || rBorrowed_.load())) return;
        prev_.store(Prev::Idle, std::memory_order_release);
        if (rBorrowed_.load()) recArmed_.store(false, std::memory_order_release);
        p.sleepMs(kRtLeaveMs);        // the render may be inside mix() or take()
        for (int i = 0; i < kDropTries; ++i) {
            uint8_t e = kRingFree;
            if (ringUse_.compare_exchange_strong(e, kRingLost)) {
                if (!pBorrowed_.load() && !rBorrowed_.load()) ringUse_.store(kRingFree);   // given back meanwhile
                logf(true, "reclaim: worker busy on the card, lent ring dropped");
                return;
            }
            p.sleepMs(1);
        }
        logf(true, "reclaim: lent ring still in use after 1 s");
    }

    // ── Jobs ──────────────────────────────────────────────────────────────
    void finish(bool ok, const std::string& result) {
        {
            std::lock_guard<std::mutex> l(resultMutex_);
            result_ = result;
        }
        progress_.store(100);
        job_.store(ok ? ToolJob::Ready : ToolJob::Failed);
    }

    void runJob(JobKind k) {
        switch (k) {
            case JobKind::List:       doList();       break;
            case JobKind::Folder:     doFolder();     break;
            case JobKind::Overview:   doOverview();   break;
            case JobKind::SaveRegion: doSaveRegion(); break;
            case JobKind::Export:     doExport();     break;
            case JobKind::Rendered:   doRendered();   break;
            case JobKind::Normalize:  doNormalize();  break;
            default: finish(false, "nothing asked"); break;
        }
    }

    void doList() {
        std::vector<std::string> out;
        if (p.storageReady()) {
            const std::string dir = recDir();
            fsx::fs().forEach(dir, [&](const char* e, bool) {
                const std::string n = e;
                if (n.size() < 5 || n[0] == '.') return;
                std::string ext = n.substr(n.size() - 4);
                for (auto& c : ext) c = (char)tolower((unsigned char)c);
                if (ext == ".wav") out.push_back(dir + "/" + n);
            });
        }
        std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return a > b; });
        {
            std::lock_guard<std::mutex> l(resultMutex_);
            recordings_ = std::move(out);
        }
        finish(true, "");
    }

    /// A folder on the card: its folders, then its WAVs, each by name, never
    /// above the card and never more than CardFolder::kMaxEntries. Names go
    /// into one block: small allocations come from internal RAM on the board.
    void doFolder() {
        std::string dir;
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            dir = pFile_;
        }
        const std::string root = p.cardRoot();
        const std::string recs = root + kRecSubdir;
        const size_t rootLen = root.size();
        if (dir.empty()) dir = recs;
        while (dir.size() > rootLen && dir.back() == '/') dir.pop_back();
        if (dir.compare(0, rootLen, root) != 0 || (dir.size() > rootLen && dir[rootLen] != '/') ||
            dir.find("/..") != std::string::npos) {
            dir = root;
        }
        if (!p.storageReady()) { finish(false, "no card"); return; }
        std::string blob;
        std::vector<uint32_t> offs;
        std::vector<uint8_t> isDir;
        blob.reserve(4096);
        offs.reserve(256);
        isDir.reserve(256);
        bool truncated = false;
        uint32_t seen = 0;
        const auto entry = [&](const char* name, bool folder) {
            if (name[0] == '.') return true;
            if (!folder && !is_wav(name)) return true;
            if (offs.size() >= CardFolder::kMaxEntries) { truncated = true; return false; }
            offs.push_back((uint32_t)blob.size());
            isDir.push_back(folder ? 1 : 0);
            blob.append(name);
            blob.push_back('\0');
            if (++seen % kDirServiceEvery == 0) giveWay();
            return true;
        };
        bool opened = fsx::fs().forEach(dir, entry);
        if (!opened && dir == recs) opened = fsx::fs().forEach(dir = root, entry);   // no take recorded yet
        if (!opened) { finish(false, "cannot open " + dir); return; }
        std::vector<uint32_t> order(offs.size());
        std::iota(order.begin(), order.end(), 0u);
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            if (isDir[a] != isDir[b]) return isDir[a] > isDir[b];
            return fsx::compareNoCase(blob.c_str() + offs[a], blob.c_str() + offs[b]) < 0;
        });
        CardFolder f;
        f.path = dir;
        f.truncated = truncated;
        f.names.reserve(blob.size());
        f.offsets.reserve(order.size());
        for (uint32_t i : order) {
            f.offsets.push_back((uint32_t)f.names.size());
            f.names.append(blob.c_str() + offs[i]);
            f.names.push_back('\0');
            if (isDir[i]) ++f.folders;
        }
        {
            std::lock_guard<std::mutex> l(resultMutex_);
            folder_ = std::move(f);
        }
        finish(true, dir);
    }

    void doOverview() {
        std::string file;
        uint32_t a, b;
        uint16_t cols;
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            file = pFile_; a = pA_; b = pB_; cols = pCols_;
        }
        ClipReader r;
        if (!r.open(file, p.storageReady())) { finish(false, "cannot read " + base_of(file)); return; }
        if (!b || b > r.frames) b = r.frames;
        if (a >= b) { r.close(); finish(false, "empty range"); return; }
        const uint32_t len = b - a;
        WaveOverview ov;
        ov.file = file;
        ov.frames = r.frames;
        ov.sampleRate = sampleRate_;
        ov.start = a;
        ov.end = b;
        ov.hop = kEnvHop;
        std::vector<int16_t> lo(cols, INT16_MAX), hi(cols, INT16_MIN);
        const size_t hops = (len + kEnvHop - 1) / kEnvHop;
        ov.envelope.assign(hops, 0);
        float acc = 0.0f;
        uint32_t accN = 0;
        size_t hopIdx = 0;
        for (uint32_t f = a; f < b;) {
            const uint32_t n = std::min(kChunk, b - f);
            r.read(f, io_, n);
            for (uint32_t k = 0; k < n; ++k) {
                const int32_t m = ((int32_t)io_[k * 2] + (int32_t)io_[k * 2 + 1]) / 2;
                const uint32_t rel = f + k - a;
                const uint32_t c = (uint32_t)((uint64_t)rel * cols / len);
                if (m < lo[c]) lo[c] = (int16_t)m;
                if (m > hi[c]) hi[c] = (int16_t)m;
                const float v = (float)m * (1.0f / 32768.0f);
                acc += v * v;
                if (++accN == kEnvHop) {
                    ov.envelope[hopIdx++] = SliceMath::envelopeStep(acc / (float)accN);
                    acc = 0.0f;
                    accN = 0;
                }
            }
            f += n;
            progress_.store((uint8_t)((uint64_t)(f - a) * 99 / len));
            giveWay();
        }
        if (accN && hopIdx < hops) ov.envelope[hopIdx] = SliceMath::envelopeStep(acc / (float)accN);
        r.close();
        ov.lo.resize(cols);
        ov.hi.resize(cols);
        for (uint16_t c = 0; c < cols; ++c) {
            ov.lo[c] = lo[c] == INT16_MAX ? 0 : (int8_t)(lo[c] / kPeakScale);
            ov.hi[c] = hi[c] == INT16_MIN ? 0 : (int8_t)(hi[c] / kPeakScale);
        }
        {
            std::lock_guard<std::mutex> l(resultMutex_);
            overview_ = std::move(ov);
        }
        finish(true, file);
    }

    /// Played frames [at, at + n) of [a, a + len) of @p r at @p ratio into
    /// @p out: read straight at unison, else through a window of just that
    /// region (SliceMath::readPitched), so a slice never hears its neighbours.
    /// @p reverse reads it from its last frame back.
    void readPlayed(ClipReader& r, uint32_t a, uint32_t len, double ratio, uint32_t at, int16_t* out, uint32_t n,
                    bool reverse = false) {
        if (ratio == 1.0 && !reverse) { r.read(a + at, out, n); return; }
        const double step = reverse ? -ratio : ratio;
        const double p0 = reverse ? (double)len - 1.0 - (double)at * ratio : (double)at * ratio;
        const double pn = p0 + (double)(n - 1) * step;
        const int64_t first = std::max<int64_t>(0, (int64_t)std::floor(std::min(p0, pn)) - 1);
        const int64_t last = std::min<int64_t>(len, (int64_t)std::max(p0, pn) + 3);
        const uint32_t w = (uint32_t)std::max<int64_t>(1, last - first);
        r.read(a + (uint32_t)first, win_, w);
        SliceMath::readPitched(win_, w, p0 - (double)first, step, out, n);
    }

    /// [a, b) of @p r into @p path: as it is with @p attack 0 and @p ratio 1,
    /// else played at @p ratio (backwards with @p reverse), through @p filters
    /// when given, and, with @p attack, as a slice rising over that many
    /// frames, with its fades.
    bool copyRegion(ClipReader& r, uint32_t a, uint32_t b, const std::string& path, uint32_t attack,
                    double ratio, uint32_t doneBefore, uint32_t total, uint32_t rate = 0,
                    SampleFilters* filters = nullptr, bool reverse = false) {
        WavWriter w;
        if (!w.open(path.c_str(), rate ? rate : sampleRate_, 2)) return false;
        const uint32_t len = b - a;
        const uint32_t out = SliceMath::pitchedFrames(len, ratio);
        for (uint32_t o = 0; o < out;) {
            const uint32_t n = std::min(kChunk, out - o);
            readPlayed(r, a, len, ratio, o, io_, n, reverse);
            if (filters) filters->process(io_, n);
            if (attack) shape(io_, n, o, out, attack);
            if (!w.write(io_, n)) { w.close(); return false; }
            o += n;
            /* Progress in source frames, whatever the pitch makes of them. */
            const uint32_t used = std::min(len, (uint32_t)((double)o * ratio));
            if (total) progress_.store((uint8_t)((uint64_t)(doneBefore + used) * 99 / total));
            giveWay();
        }
        return w.close();
    }

    void doSaveRegion() {
        std::string file;
        uint32_t a, b;
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            file = pFile_; a = pA_; b = pB_;
        }
        ClipReader r;
        if (!r.open(file, p.storageReady())) { finish(false, "cannot read " + base_of(file)); return; }
        if (!b || b > r.frames) b = r.frames;
        if (a >= b) { r.close(); finish(false, "empty range"); return; }
        /* Into the recordings, wherever the source is: a loop cut from a kit's
         * sample must not land in that kit's folder. */
        if (!makeRecDir()) { r.close(); finish(false, "no card"); return; }
        const std::string out = next_free(recDir(), "loop");
        if (out.empty()) { r.close(); finish(false, "no free name"); return; }
        const bool ok = copyRegion(r, a, b, out, 0, 1.0, 0, b - a);
        r.close();
        if (!ok) { fsx::remove(out); finish(false, "card write failed"); return; }
        finish(true, out);
    }

    /// A sample as a pad with pitch or filters plays it: rendered beside,
    /// swapped in, so a pad never plays half a file. One already there is kept
    /// -- a file per setting, and the sample it was made from does not change
    /// under it.
    void doRendered() {
        std::string file, out;
        SampleFx fx;
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            file = pFile_; out = pDst_; fx = pFx_;
        }
        if (fsx::exists(out)) { finish(true, out); return; }
        ClipReader r;
        if (!r.open(file, p.storageReady())) { finish(false, "cannot read " + base_of(file)); return; }
        const size_t slash = out.find_last_of('/');
        if (slash == std::string::npos || !fsx::makeDirs(out.substr(0, slash))) {
            r.close();
            finish(false, "cannot make " + out.substr(0, slash));
            return;
        }
        const std::string tmp = out + ".tmp";
        SampleFilters filters;
        filters.set(fx, r.rate ? r.rate : sampleRate_);
        const bool ok = fx.keepLength && fx.semitones
                            ? renderKeepingLength(r, fx, tmp, filters)
                            : copyRegion(r, 0, r.frames, tmp, 0, SliceMath::pitchRatio(fx.semitones), 0, r.frames,
                                         r.rate, &filters, fx.reverse);
        r.close();
        if (!ok || !fsx::rename(tmp, out)) {
            fsx::remove(tmp);
            finish(false, "card write failed");
            return;
        }
        logf(false, "%s %+d st%s hp %u lp %u%s -> %s", base_of(file).c_str(), fx.semitones,
             fx.keepLength && fx.semitones ? " (length kept)" : "", (unsigned)fx.hpHz, (unsigned)fx.lpHz,
             fx.reverse ? " reversed" : "", out.c_str());
        finish(true, out);
    }

    /// What the TimeStretch callbacks of one pass work on.
    struct StretchPass {
        Impl*          self;
        ClipReader*    in;
        WavWriter*     out;
        SampleFilters* filters;      ///< pass two: on the way out
        bool           reverse;      ///< pass one: the input read from its end
        uint32_t       pctFrom;      ///< where this pass's progress starts, of 99
        uint32_t       pctSpan;
    };

    static bool stretch_read(void* c, uint32_t frame, int16_t* dst, uint32_t n) {
        auto* sp = static_cast<StretchPass*>(c);
        if (!sp->reverse) { sp->in->read(frame, dst, n); return true; }
        /* Frames [frame, frame + n) of the file backwards: [len - frame - n,
         * len - frame) forwards, turned round. TimeStretch asks only inside it. */
        sp->in->read(sp->in->frames - frame - n, dst, n);
        for (uint32_t a = 0, b = n - 1; a < b; ++a, --b) {
            std::swap(dst[a * 2], dst[b * 2]);
            std::swap(dst[a * 2 + 1], dst[b * 2 + 1]);
        }
        return true;
    }

    static bool stretch_write(void* c, const int16_t* src, uint32_t n) {
        auto* sp = static_cast<StretchPass*>(c);
        if (!sp->filters) return sp->out->write(src, n);
        int16_t* io = sp->self->io_;           // kChunk frames, a hop is fewer
        std::memcpy(io, src, (size_t)n * 4);
        sp->filters->process(io, n);
        return sp->out->write(io, n);
    }

    static bool stretch_keep(void* c, uint32_t done, uint32_t total) {
        auto* sp = static_cast<StretchPass*>(c);
        sp->self->progress_.store((uint8_t)(sp->pctFrom + (uint64_t)done * sp->pctSpan / total));
        sp->self->giveWay();
        return true;
    }

    /// @p r transposed by @p fx's semitones, its length kept: stretched by the
    /// pitch's ratio into a file of its own (the pitch as it was), then that
    /// read back at the ratio into exactly @p r's frames -- through
    /// @p filters -- at @p path. Two passes over the card, half the progress
    /// each; the working memory is borrowed for the run and given back.
    bool renderKeepingLength(ClipReader& r, const SampleFx& fx, const std::string& path, SampleFilters& filters) {
        if (!r.frames) return false;
        const uint32_t rate = r.rate ? r.rate : sampleRate_;
        const uint32_t stretched = (uint32_t)std::max<double>(
            1.0, std::round((double)r.frames * SliceMath::pitchRatio(fx.semitones)));
        bool lent = false;
        void* work = p.allocRingSegment(TimeStretch::workBytes(), 1, false, lent);
        if (!work) return false;
        const std::string mid = path + ".str";
        WavWriter w;
        StretchPass one{this, &r, &w, nullptr, fx.reverse, 0, 49};
        bool ok = w.open(mid.c_str(), rate, 2) &&
                  TimeStretch::run(r.frames, stretched, work, stretch_read, stretch_write, stretch_keep, &one);
        ok = w.close() && ok;
        ClipReader m;
        if (ok) ok = m.open(mid, p.storageReady());
        if (ok) {
            WavWriter w2;
            StretchPass two{this, &m, &w2, filters.on() ? &filters : nullptr, false, 49, 50};
            ok = w2.open(path.c_str(), rate, 2) &&
                 TimeStretch::repitch(m.frames, r.frames, work, stretch_read, stretch_write, stretch_keep, &two);
            ok = w2.close() && ok;
        }
        m.close();
        fsx::remove(mid);
        p.freeRingSegment(work);
        return ok;
    }

    /// A sample brought up to the asked peak: one pass finds its loudest
    /// sample, a second writes it scaled, beside and swapped in as a rendered
    /// copy is. Never turned down -- one at or above the peak already is the
    /// answer itself, and nothing is written.
    void doNormalize() {
        std::string file, out;
        float peak;
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            file = pFile_; out = pDst_; peak = pPeak_;
        }
        if (fsx::exists(out)) { finish(true, out); return; }
        ClipReader r;
        if (!r.open(file, p.storageReady())) { finish(false, "cannot read " + base_of(file)); return; }
        if (!r.frames) { r.close(); finish(false, "the sample is empty"); return; }
        int32_t top = 0;
        for (uint32_t f = 0; f < r.frames;) {
            const uint32_t n = std::min(kChunk, r.frames - f);
            r.read(f, io_, n);
            for (uint32_t k = 0; k < n * 2; ++k) {
                const int32_t v = io_[k] < 0 ? -(int32_t)io_[k] : io_[k];
                if (v > top) top = v;
            }
            f += n;
            progress_.store((uint8_t)((uint64_t)f * kNormalizeScanPct / r.frames));
            giveWay();
        }
        if (!top) { r.close(); finish(false, "the sample is silent"); return; }
        const float gain = peak * (float)INT16_MAX / (float)top;
        if (gain < kNormalizeSlack) {
            r.close();
            finish(true, file);
            return;
        }
        const std::string tmp = out + ".tmp";
        WavWriter w;
        bool ok = w.open(tmp.c_str(), r.rate ? r.rate : sampleRate_, 2);
        for (uint32_t f = 0; ok && f < r.frames;) {
            const uint32_t n = std::min(kChunk, r.frames - f);
            r.read(f, io_, n);
            for (uint32_t k = 0; k < n * 2; ++k) {
                const float v = std::round((float)io_[k] * gain);
                io_[k] = (int16_t)(v > (float)INT16_MAX ? (float)INT16_MAX : (v < (float)INT16_MIN ? (float)INT16_MIN : v));
            }
            ok = w.write(io_, n);
            f += n;
            progress_.store((uint8_t)(kNormalizeScanPct + (uint64_t)f * kNormalizeWritePct / r.frames));
            giveWay();
        }
        ok = w.close() && ok;
        r.close();
        if (!ok || !fsx::rename(tmp, out)) {
            fsx::remove(tmp);
            finish(false, "card write failed");
            return;
        }
        logf(false, "%s peak %ld, x%.2f (%+.1f dB) -> %s", base_of(file).c_str(), (long)top, (double)gain,
             20.0 * std::log10((double)gain), out.c_str());
        finish(true, out);
    }

    void doExport() {
        SliceKit kit;
        {
            std::lock_guard<std::mutex> l(paramMutex_);
            kit = pKit_;
        }
        const std::string kitsDir = p.cardRoot() + kSlicerKits;
        const std::string dir = kitsDir + "/" + kit.name;
        if (!p.storageReady()) { finish(false, "no card"); return; }
        if (fsx::exists(dir)) { finish(false, "a kit called " + kit.name + " is already on the card"); return; }
        ClipReader r;
        if (!r.open(kit.source, p.storageReady())) { finish(false, "cannot read " + base_of(kit.source)); return; }
        uint32_t end = kit.end && kit.end <= r.frames ? kit.end : r.frames;
        std::vector<uint32_t> starts = kit.starts;
        std::sort(starts.begin(), starts.end());
        starts.erase(std::remove_if(starts.begin(), starts.end(), [end](uint32_t s) { return s >= end; }), starts.end());
        if (starts.empty()) { r.close(); finish(false, "no slices"); return; }
        if (!fsx::makeDir(kitsDir) || !fsx::makeDir(dir) || !fsx::makeDir(dir + "/SAMPLES")) {
            r.close();
            finish(false, "cannot make the kit's folder");
            return;
        }
        /* A Slice kit: the loop once, as it is, and each pad a region of it.
         * Pitch and attack are the kit's, played by the engine in real time;
         * it used to be a file per slice with both written into it. */
        const uint32_t base = starts.front();
        static constexpr const char* kLoop = "loop.wav";
        if (!copyRegion(r, base, end, dir + "/SAMPLES/" + kLoop, 0, 1.0, 0, end - base)) {
            r.close();
            finish(false, "card write failed");
            return;
        }
        r.close();

        KitInfo info;
        info.name = kit.name;
        info.author = "CrossPad Slicer";
        info.category = "slice";
        info.engine = KitEngine::Slice;
        info.pitch = kit.semitones;
        info.description = "Sliced from " + base_of(kit.source);
        info.version = "1.0";
        /* Varispeed moves the tempo with the pitch. */
        info.bpm = (uint16_t)std::lround((double)kit.bpm * SliceMath::pitchRatio(kit.semitones));
        info.path = std::string(kSlicerKits) + "/" + kit.name + "/kit.json";
        for (int pad = 0; pad < KIT_PADS; ++pad) {
            Pad& kp = info.pads[pad];
            kp.color = kp.color_pressed = slice_color(pad);
            if (pad >= (int)starts.size()) continue;
            const uint32_t a = starts[pad];
            const uint32_t b = pad + 1 < (int)starts.size() ? starts[pad + 1] : end;
            kp.attack = kit.attackMs;
            kp.AddSample(kLoop, false);
            kp.samples.back().start = a - base;
            kp.samples.back().end = b - base;
        }
        IKitManager* kits = getKitManager();
        if (!kits || !kits->saveKit(info)) {                 // to info.path, under the kit manager's root
            finish(false, "kit.json not written");
            return;
        }
        const int id = kits->addKit(info.path);
        logf(false, "kit %s: %u slices, list id %d", kit.name.c_str(), (unsigned)starts.size(), id);
        finish(true, id >= 0 ? kit.name : kit.name + " (in the kit list after a restart)");
    }

    ISampleToolsPlatform& p;

    // Input and take.
    std::mutex            inputMutex_;
    std::atomic<RecInput> input_{RecInput::Mic};
    std::atomic<bool>     listening_{false};
    uint8_t               claimed_ = 0;                       // codecs whose ADC is held, a bit each; under inputMutex_
    std::atomic<int32_t>  pkL_{0}, pkR_{0};
    std::atomic<bool>     recArmed_{false}, recStartReq_{false}, recStopReq_{false}, recOverflow_{false};
    std::atomic<uint32_t> recFrames_{0};
    bool                  overflowWrite_ = false;             // worker
    bool                  recWithSong_ = false;               // set before recStartReq_
    uint32_t              recFromBar_ = 0;
    uint8_t               recCountIn_ = 0;
    std::atomic<CaptureTap> captureTap_{nullptr};
    void*                 captureTapCtx_ = nullptr;

    // A take with the song.
    std::atomic<bool>     syncWait_{false};                   // armed, the song not heard yet
    std::atomic<bool>     syncSeenIdle_{false};               // render saw the song silent since arming
    std::atomic<uint32_t> syncFrame_{0};                      // take frame its first frame came in with
    /* A take into a song already playing (rtPunch): its bar, its loop's end
     * and where it wraps to, and the bar frame 0 went in at. */
    std::atomic<bool>     punch_{false};
    std::atomic<bool>     punchLoop_{false};
    std::atomic<uint32_t> punchSpb_{0};
    std::atomic<uint32_t> punchLim_{0};
    std::atomic<uint32_t> punchWrap_{0};
    std::atomic<uint32_t> punchBar_{0};
    std::atomic<uint32_t> skip_{0};                           // frames before frame 0; UINT32_MAX = not known
    uint32_t              blockBegin_ = 0;                    // render: take frames when this block began
    bool                  syncTake_ = false;                  // worker
    uint32_t              dropped_ = 0;                       // worker: lead-in thrown away
    int64_t               armedUs_ = 0;                       // worker
    FrameRing             rRing_;
    WavWriter             writer_;                            // worker
    RecStatus             rec_;
    std::mutex            statusMutex_;
    std::vector<int16_t>  capture_;                           // render: the pads bus as int16
    std::vector<int16_t>  held_;                              // render: MicLine, the mics until the line-in comes
    uint32_t              heldFrames_ = 0;                    // render: frames in held_, 0 = none

    // Preview.
    std::atomic<Prev>     prev_{Prev::Idle};
    std::atomic<bool>     prevStopReq_{false};
    std::atomic<uint32_t> prevGen_{0}, prevDone_{0};          // requests made, and handled
    std::string           prevFile_;                          // under paramMutex_
    uint32_t              prevA_ = 0, prevB_ = 0;
    bool                  prevLoop_ = false;
    uint32_t              prevAttack_ = 0;
    double                prevRatio_ = 1.0;
    SampleFx              prevFx_;
    FrameRing             pRing_;
    std::atomic<bool>     pBorrowed_{false};                  // pRing_.borrowed, for reclaimLentRing()
    std::atomic<bool>     rBorrowed_{false};                  // rRing_.borrowed, for reclaimLentRing()
    std::atomic<bool>     takeCut_{false};                    // a reclaim took the take's ring
    std::atomic<uint8_t>  ringUse_{kRingFree};                // see ringEnter()
    std::atomic<bool>     reclaimReq_{false};                 // reclaimLentRing() asks the worker
    ClipReader            pReader_;                           // worker
    uint32_t              pStart_ = 0, pLen_ = 0, pWrite_ = 0;
    bool                  pLoop_ = false;
    uint32_t              pAttack_ = 0;                       // worker: 0 = as it is, else a slice's rise
    SampleFilters         pFilters_;                          // worker: the preview's high and low pass
    bool                  pReverse_ = false;                  // the preview runs from its end back
    double                pRatio_ = 1.0;                      // file frames per played frame
    uint32_t              pOut_ = 0;                          // played frames of [pStart_, pStart_ + pLen_)
    std::atomic<uint32_t> pDone_{0};
    std::vector<int16_t>  scratch_;

    // Jobs.
    std::atomic<ToolJob>  job_{ToolJob::Idle};
    std::atomic<JobKind>  jobKind_{JobKind::None};
    std::atomic<uint8_t>  progress_{0};
    std::mutex            paramMutex_, resultMutex_;
    std::string           pFile_;
    std::string           pDst_;                              // Rendered: where the copy goes
    SampleFx              pFx_;
    float                 pPeak_ = 1.0f;                      // Normalize: the loudest sample, of full scale
    uint32_t              pA_ = 0, pB_ = 0;
    uint16_t              pCols_ = 1;
    SliceKit              pKit_;
    std::string           hFile_;
    uint32_t              hA_ = 0, hB_ = 0;
    std::string           result_;
    std::vector<std::string> recordings_;
    CardFolder            folder_;
    WaveOverview          overview_;

    uint32_t              sampleRate_ = 44100;
    int16_t*              io_ = nullptr;
    int16_t*              win_ = nullptr;                     // worker: the source under a pitched chunk
    std::mutex            workerMutex_;
    std::atomic<bool>     workerUp_{false};
    std::atomic<bool>     quit_{false};
};

SampleToolsEngine::SampleToolsEngine(ISampleToolsPlatform& platform) : impl_(new Impl(platform)) {}

SampleToolsEngine::~SampleToolsEngine()
{
    impl_->shutdown();
}

const char* SampleToolsEngine::name() const { return "Preview"; }
bool SampleToolsEngine::isGenerator() const { return true; }
void SampleToolsEngine::onPrepare(uint32_t maxFrames, uint32_t sampleRate) { impl_->onPrepare(maxFrames, sampleRate); }
void SampleToolsEngine::onSampleRateChange(uint32_t sr) { impl_->onSampleRateChange(sr); }
void SampleToolsEngine::mix(float* out, uint32_t frames) { impl_->mix(out, frames); }

bool SampleToolsEngine::wantsCodec(int codec) const { return impl_->wantsCodec(codec); }
void SampleToolsEngine::rtInput(int codec, const int16_t* block, uint32_t frames) { impl_->rtInput(codec, block, frames); }
void SampleToolsEngine::rtUsb(const int16_t* block, uint32_t frames) { impl_->rtUsb(block, frames); }
void SampleToolsEngine::rtBlockBegin() { impl_->rtBlockBegin(); }
void SampleToolsEngine::rtBus(const float* padsBus, uint32_t frames) { impl_->rtBus(padsBus, frames); }
bool SampleToolsEngine::wantsBus() const { return impl_->wantsBus(); }
void SampleToolsEngine::setCaptureTap(CaptureTap fn, void* ctx) { impl_->setCaptureTap(fn, ctx); }
void SampleToolsEngine::reclaimLentRing() { impl_->reclaimLentRing(); }

bool SampleToolsEngine::selectInput(RecInput in) { return impl_->selectInput(in); }
RecInput SampleToolsEngine::input() const { return impl_->input_.load(); }
bool SampleToolsEngine::inputAvailable(RecInput in) const { return impl_->inputAvailable(in); }
void SampleToolsEngine::peaks(float& left, float& right) { impl_->peaks(left, right); }
bool SampleToolsEngine::startRecording() { return impl_->requestTake(false, 0, 0); }
bool SampleToolsEngine::startRecordingWithSong(uint32_t fromBar) { return startRecordingWithSong(fromBar, 0); }
bool SampleToolsEngine::startRecordingWithSong(uint32_t fromBar, uint8_t countInBeats)
{
    return getPlatformServices().songEngine && impl_->requestTake(true, fromBar, countInBeats);
}
void SampleToolsEngine::stopRecording() { impl_->stopRecording(); }
RecStatus SampleToolsEngine::recStatus() { return impl_->recStatus(); }
void SampleToolsEngine::releaseInput() { impl_->releaseInput(); }

bool SampleToolsEngine::requestRecordings() { return impl_->request(JobKind::List); }
std::vector<std::string> SampleToolsEngine::recordings() { return impl_->recordings(); }
bool SampleToolsEngine::requestOverview(const std::string& file, uint32_t start, uint32_t end, uint16_t columns)
{
    return impl_->requestOverview(file, start, end, columns);
}
WaveOverview SampleToolsEngine::overview() { return impl_->overview(); }
bool SampleToolsEngine::requestFolder(const std::string& dir) { return impl_->requestFolder(dir); }
CardFolder SampleToolsEngine::folder() { return impl_->folder(); }
std::string SampleToolsEngine::cardRoot() const { return impl_->p.cardRoot(); }
bool SampleToolsEngine::requestSaveRegion(const std::string& file, uint32_t start, uint32_t end)
{
    return impl_->requestSaveRegion(file, start, end);
}
bool SampleToolsEngine::requestExportKit(const SliceKit& kit) { return impl_->requestExportKit(kit); }
bool SampleToolsEngine::requestRendered(const std::string& file, const std::string& out, const SampleFx& fx)
{
    return impl_->requestRendered(file, out, fx);
}
bool SampleToolsEngine::requestNormalized(const std::string& file, const std::string& out, float peak)
{
    return impl_->requestNormalized(file, out, peak);
}
ToolJob SampleToolsEngine::job() const { return impl_->job_.load(); }
std::string SampleToolsEngine::jobResult() { return impl_->jobResult(); }
uint8_t SampleToolsEngine::jobProgress() const { return impl_->progress_.load(); }

bool SampleToolsEngine::preview(const std::string& file, uint32_t start, uint32_t end, bool loop)
{
    return impl_->previewAs(file, start, end, loop, PreviewSound{});
}
bool SampleToolsEngine::previewAs(const std::string& file, uint32_t start, uint32_t end, bool loop,
                                  const PreviewSound& sound)
{
    return impl_->previewAs(file, start, end, loop, sound);
}
void SampleToolsEngine::stopPreview() { impl_->stopPreview(); }
bool SampleToolsEngine::previewing() const { return impl_->previewing(); }
uint32_t SampleToolsEngine::previewFrame() const { return impl_->previewFrame(); }

void SampleToolsEngine::setHandoff(const std::string& file, uint32_t start, uint32_t end)
{
    impl_->setHandoff(file, start, end);
}
bool SampleToolsEngine::takeHandoff(std::string& file, uint32_t& start, uint32_t& end)
{
    return impl_->takeHandoff(file, start, end);
}

} // namespace crosspad

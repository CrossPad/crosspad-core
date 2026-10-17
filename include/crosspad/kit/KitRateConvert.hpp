// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file KitRateConvert.hpp
 * @brief A kit's samples rewritten at the engine's rate, once and for good.
 *
 * The sampler plays a file at any rate by stepping its phase, so a 48 kHz kit
 * on a 44.1 kHz engine interpolates every voice: 66-73 us a voice against
 * 18-21 at the engine's own rate (hardware, 16.10.2026) -- the difference between
 * a storm the render keeps up with and one it does not. Converting the files
 * removes that cost for good.
 *
 * **Nothing the user has is ever lost on the way.** The converted files are
 * written beside the originals, in a folder of the kit named for the rate
 * (`<kit>/44k1/...`), each one verified before it counts. The kit only starts
 * using them at one moment: the atomic replace of its JSON (`.tmp` / `.bak`,
 * as every kit save), which retargets the samples and scales their trims. Cut
 * the power anywhere before that and the kit is the old one, untouched; after
 * it, the new one. A conversion that was stopped resumes: a converted file
 * that verifies is kept, a `.part` is written again.
 *
 * The originals go only after that, and only those inside the kit's own
 * folder that nothing names any more -- not this kit's JSON, not another
 * kit's (`tidyKit()`). A file the kit borrows from elsewhere on the card is
 * copied, never touched. Until the tidy, `<kit>/<rate>/.originals` lists what
 * it still has to remove, so a tidy that was cut short finishes later.
 *
 * Portable: stdio, and the board's specifics (where memory comes from, how a
 * write reaches the card, giving way to the streamer, the free space) behind
 * IKitConvertHost.
 */

#include "crosspad/audio/WavLayout.hpp"
#include "crosspad/dsp/IFft.hpp"

#include <ArduinoJson.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace crosspad {

/// Room rateFolderName() needs.
inline constexpr size_t kRateFolderMax = 12;

/// The folder of a kit that holds its samples at @p rate: "44k1", "48k",
/// "22k05". Written into @p buf (kRateFolderMax), which it returns.
const char* rateFolderName(uint32_t rate, char* buf);

/// A frame position at @p inRate as the same moment at @p outRate, rounded.
uint32_t scaleFrame(uint32_t frame, uint32_t inRate, uint32_t outRate);

/**
 * @brief Move the sample positions a metadata chunk carries to @p outRate.
 *
 * `smpl` (loops, sample period), `cue ` (points), `LIST`/`adtl` (`ltxt`
 * lengths), `bext` (time reference), `fact` (length) are scaled in place.
 * @return false for a chunk that must not be copied at all: `PEAK` (levels
 *         and positions of a signal that no longer exists) and padding
 *         (`JUNK`, `PAD `, `FLLR`). Everything else is copied as it is.
 */
bool scaleWavChunk(const char id[4], uint8_t* payload, uint32_t size,
                   uint32_t inRate, uint32_t outRate);

/**
 * @brief What the converter needs from the board.
 *
 * Every default is plain stdio and the C heap, which is what the host tests
 * and the simulator run on.
 */
class IKitConvertHost {
public:
    virtual ~IKitConvertHost() = default;

    /// Working memory: the resampler and the I/O buffers, 62 kB for a stereo
    /// file (test_KitRateConvert). Released when the file is done.
    virtual void* alloc(size_t bytes);
    virtual void  release(void* p);

    /// The transform the resampler runs on. The converter caps the block it
    /// asks for (kConvertFftMax), so a backend built for 8192 costs no more.
    virtual IFft& fft() = 0;

    /// Bulk transfers: the samples, a piece of at most kConvertPieceBytes.
    virtual size_t readIo(FILE* f, void* dst, size_t bytes);
    virtual bool   writeIo(FILE* f, const void* src, size_t bytes);

    /// Everything written to @p f is on the medium. Default: fflush.
    virtual bool syncFile(FILE* f);

    /// After each piece: let whatever plays from the card go first.
    virtual void giveWay() {}

    /// Free bytes on the volume holding @p fsDir; UINT64_MAX when unknown.
    virtual uint64_t freeBytes(const char* fsDir);

    virtual bool makeDir(const char* fsPath);

    /// The conversion has ended, whichever way: anything held for it (an FFT
    /// built for it, say) can go.
    virtual void endJob() {}

    /// The tidy is about to remove @p fsPath: close any handle the platform
    /// keeps open on it. False while something may still read it -- a voice
    /// of the outgoing kit streaming its tail -- and the file then stays for
    /// a later tidy.
    virtual bool releaseFile(const char* fsPath) { (void)fsPath; return true; }

    /// Where kit JSON documents live while they are edited; null = the
    /// library's default allocator.
    virtual ArduinoJson::Allocator* jsonAllocator() { return nullptr; }
};

/// The resampler's read-out oversample. A conversion is for good, so it is
/// read at 8x rather than the pitched loader's 2x: a 19 kHz tone at 48 k ->
/// 44.1 k leaves its images at -69 dB instead of -30, 10 kHz at -85
/// (test_KitRateConvert).
inline constexpr uint32_t kConvertOversample = 8;

/// Largest transform the converter asks for: the 8x inverse of a 256-sample
/// block, ~21 kB of scratch a channel -- the board has ~130 kB to spare with
/// a kit loaded, and a stereo file needs two resamplers. In the band the
/// block changes nothing (the same decibel from 256 to 4096); what it sets is
/// the transition above the new Nyquist: a full-scale 23 kHz tone folds to
/// 21.1 kHz at -57 dB here, -69 at 512, -85 at 1024.
inline constexpr uint32_t kConvertFftMax = 2048;

/// Largest single read or write the converter issues: the board's DMA stage.
inline constexpr uint32_t kConvertPieceBytes = 4096;

/// One sample entry of the kit JSON that the conversion retargeted: its pad,
/// what it named and its trim, and what it names now with the trim scaled.
/// The kit model in memory is brought in line from these -- an entry of that
/// pad saying the same old thing -- without reading the card.
struct KitConvertEdit {
    uint8_t     pad = 0;
    std::string oldSample;
    uint32_t    oldStart = 0;
    uint32_t    oldEnd = 0;
    std::string newSample;
    uint32_t    start = 0;
    uint32_t    end = 0;
};

class KitRateConverter {
public:
    enum class Phase : uint8_t {
        Idle,
        Planning,     ///< reading the kit and every sample's header
        Converting,   ///< writing the converted files, one piece a step
        Committing,   ///< the kit's JSON swapped to the converted files
        Done,
        Failed,
        Cancelled,
    };

    struct Progress {
        Phase    phase = Phase::Idle;
        uint16_t files = 0;          ///< distinct samples of the kit
        uint16_t toConvert = 0;      ///< not at the target rate, and convertible
        uint16_t converted = 0;      ///< written and verified (this run or one before)
        uint16_t skipped = 0;        ///< not 16-bit PCM, unreadable, or changed meanwhile
        uint16_t fileIndex = 0;      ///< the one being written, 1-based
        uint32_t framesDone = 0;     ///< of the files left to write, in source frames
        uint32_t framesTotal = 0;
        uint32_t fromRate = 0;       ///< the most common rate converted from
        uint32_t toRate = 0;
        char     current[48] = {};   ///< the file being written
        char     error[48] = {};

        /// 0..100 over the whole job.
        uint8_t percent() const;
    };

    KitRateConverter() = default;
    ~KitRateConverter();
    KitRateConverter(const KitRateConverter&) = delete;
    KitRateConverter& operator=(const KitRateConverter&) = delete;

    /// @p kitJsonPath is logical ("/crosspad/kits/X/kit.json"); the card path
    /// comes from kitFsPath(). Starts planning; drive it with step().
    bool begin(IKitConvertHost& host, const std::string& kitJsonPath, uint32_t targetRate);

    /// One piece of work: a header while planning, a block of frames while
    /// converting, the JSON swap. False once it has finished, whichever way.
    bool step();

    /// From any thread. The file being written is dropped; the ones finished
    /// stay, and the next conversion of this kit picks them up.
    void cancel() { cancel_.store(true, std::memory_order_relaxed); }

    Phase phase() const { return progress_.phase; }
    /// The step() thread's view; another thread copies it under its own lock.
    const Progress& progress() const { return progress_; }
    /// The entries the commit retargeted (Phase::Done).
    const std::vector<KitConvertEdit>& edits() const { return edits_; }

private:
    enum class FileState : uint8_t { Pending, Done, Skip };

    /// A distinct sample. Paths are offsets into names_: one allocation for
    /// all of them -- small strings come out of internal RAM on the board.
    struct FileJob {
        uint32_t src = 0;        ///< card path of the original
        uint32_t dst = 0;        ///< card path of the converted file
        uint32_t rel = 0;        ///< what kit.json calls the converted file
        uint32_t srcBytes = 0;
        uint32_t rate = 0;
        uint32_t frames = 0;
        uint8_t  channels = 0;
        FileState state = FileState::Pending;
        bool     inside = false; ///< in the kit's own folder: the tidy's to remove
    };

    const char* name(uint32_t off) const { return names_.c_str() + off; }
    uint32_t addName(const char* s);

    bool stepOnce();
    bool planKit();
    bool planFile(FileJob& f);
    bool planDone();
    bool startFile();
    bool stepFile();
    bool emit(uint32_t frames);
    bool copyChunks(bool afterData);
    bool finishFile();
    void dropFile();
    bool commit();
    bool fail(const char* why);
    void endFileIo();

    IKitConvertHost* host_ = nullptr;
    std::string kitJson_;        ///< logical
    char        kitDirFs_[192] = {};
    uint32_t    target_ = 0;
    std::string names_;
    std::vector<FileJob> files_;
    std::vector<KitConvertEdit> edits_;
    size_t      cursor_ = 0;     ///< planning / converting: the file at hand
    bool        kitRead_ = false;
    Progress    progress_;
    std::atomic<bool> cancel_{false};

    /// The file being written.
    struct Io;
    Io* io_ = nullptr;
};

/// What tidyKit() did.
struct KitTidyResult {
    uint16_t removed = 0;    ///< originals deleted
    uint16_t kept = 0;       ///< still named by this kit or another: never deleted
    uint16_t pending = 0;    ///< could not be removed now (open, say): left for the next tidy
    bool     ok = false;     ///< false only when the kit or its list could not be read
};

/**
 * @brief Remove the originals a committed conversion left behind.
 *
 * Reads `<kit>/<rate>/.originals`; a file goes only when neither this kit's
 * JSON nor any other kit under @p kitsRoot (logical, "/crosspad/kits") names
 * it. Another kit can reach into this one's folder only by an absolute path
 * or one with "..", so only those entries are resolved -- no card access per
 * sample. With nothing left to remove, the list goes too. No list, nothing to
 * do: ok with all counts 0.
 */
KitTidyResult tidyKit(IKitConvertHost& host, const std::string& kitJsonPath,
                      const std::string& kitsRoot, uint32_t targetRate);

/// Whether a converted kit still has a tidy to finish (its list is on the card).
bool kitTidyPending(const std::string& kitJsonPath, uint32_t targetRate);

} // namespace crosspad

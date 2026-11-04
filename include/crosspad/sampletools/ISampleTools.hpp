// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/sampletools/RecInput.hpp"
#include "crosspad/sampletools/SampleFx.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace crosspad {

/// Where a file job (listing, overview, saving, export) is.
enum class ToolJob : uint8_t { Idle, Busy, Ready, Failed };

/// What the recorder is doing, or what its last take came to.
struct RecStatus {
    bool        recording  = false;
    uint32_t    frames     = 0;       ///< written so far, or the length of the last take
    uint32_t    sampleRate = 0;       ///< of the take, in Hz
    std::string file;                 ///< full path of the take being written or last written
    std::string error;                ///< why the last take failed; empty when it did not
    bool        withSong = false;     ///< lined up with the song: frame 0 is bar @c fromBar
    uint32_t    fromBar  = 0;         ///< the song bar frame 0 lines up with, when withSong
    bool        waiting  = false;     ///< armed with the song, which has not sounded yet
};

/// A picture of part of a WAV: min/max per column for drawing, and a level
/// envelope for finding the hits (see SliceMath).
struct WaveOverview {
    std::string          file;
    uint32_t             frames     = 0;   ///< the whole file
    uint32_t             sampleRate = 0;
    uint32_t             start = 0, end = 0;   ///< the range the columns cover
    std::vector<int8_t>  lo, hi;           ///< per column, -127..127
    uint32_t             hop = 0;          ///< frames per envelope value, from start
    std::vector<uint8_t> envelope;         ///< level per hop, SliceMath::kEnvDbPerStep dB a step
};

/// A folder on the card as the tools list it: its folders, then its WAVs,
/// each by name. The names are one block of text, not a string each, so a
/// folder of hundreds of samples costs one allocation.
struct CardFolder {
    static constexpr size_t kMaxEntries = 2000;
    std::string           path;            ///< the folder listed, full path
    std::string           names;           ///< every name, each ended by '\0'
    std::vector<uint32_t> offsets;         ///< where each name starts in @c names
    uint32_t              folders = 0;     ///< the first @c folders names are folders
    bool                  truncated = false;   ///< more than kMaxEntries: the rest is not listed
    size_t      size() const { return offsets.size(); }
    const char* name(size_t i) const { return names.c_str() + offsets[i]; }
    bool        isFolder(size_t i) const { return i < folders; }
};

/// A sliced loop to write as a kit: slice i is [starts[i], starts[i+1]),
/// the last one ends at @c end. At most 16, one per pad.
struct SliceKit {
    std::string           source;          ///< full path of the WAV
    std::string           name;            ///< the kit's folder and name
    std::vector<uint32_t> starts;          ///< sorted
    uint32_t              end = 0;
    uint16_t              bpm = 0;         ///< 0 = not known
    uint16_t              attackMs = 0;    ///< every slice's rise (SliceMath::attackFrames)
    int8_t                semitones = 0;   ///< every slice transposed, varispeed (SliceMath::pitchRatio)
    /// A PERF kit of the user's own (KitFolders::kKits, "own"): a WAV per
    /// slice with the attack and the pitch written into it. Otherwise a Slice
    /// kit (KitFolders::kSlicer): the loop once, a region a pad, both played.
    bool                  perf = false;
};

/// How a preview sounds: transposed, filtered, and/or as a kit's slice.
struct PreviewSound {
    int8_t   semitones = 0;    ///< varispeed, -12..+12 (SliceMath::pitchRatio)
    bool     slice     = false;///< rising over attackMs, with the slice's fades (SliceMath::sliceGain)
    uint16_t attackMs  = 0;
    uint16_t hpHz      = 0;    ///< high pass, 0 = off (SampleFilters)
    uint16_t lpHz      = 0;    ///< low pass, 0 = off
    bool     reverse   = false;///< from end to start; previewFrame() then runs backwards
};

/// Recording, looking at and cutting samples: the platform's side of the
/// Recorder and the Slicer.
///
/// The card is never waited for. File work is a request that returns at
/// once; job() says when it is done and jobResult() what came of it. One job
/// at a time: a request while one runs is refused. Recording and preview are
/// realtime and run beside the jobs.
class ISampleTools {
public:
    virtual ~ISampleTools() = default;

    // ── Input and recording ───────────────────────────────────────────────
    /// Listen to @p in: peaks() meters it and the next take records it. The
    /// codec input it needs is switched now, and given back by releaseInput().
    virtual bool     selectInput(RecInput in) = 0;
    /// The input selected with selectInput().
    virtual RecInput input() const = 0;
    /// Whether @p in has anything to record right now -- USB only while the
    /// board is in its USB audio profile.
    virtual bool     inputAvailable(RecInput in) const { return in != RecInput::Usb; }
    /// Peaks of the selected input since the last call, 0..1.
    virtual void     peaks(float& left, float& right) = 0;
    /// A new take in the recordings folder, stereo, at the engine's rate.
    virtual bool     startRecording() = 0;
    /// A take lined up with the song engine: armed, then the song played
    /// from @p fromBar. Frame 0 of the take is what came in with the song's
    /// first frame, the delay through the codecs taken off, so the take sits
    /// on the song's bars from @p fromBar on; nothing before it is kept.
    /// A song already playing -- started from the transport bar -- is not
    /// started again: the take goes in at the next bar line it reaches, and
    /// RecStatus::fromBar says which bar that was. stopRecording() stops the
    /// song with the take.
    virtual bool     startRecordingWithSong(uint32_t fromBar) { (void)fromBar; return false; }
    /// The same after @p countInBeats of count-in (ISongEngine::playCountedIn):
    /// frame 0 is still the song's first, on the beat after the last click.
    virtual bool     startRecordingWithSong(uint32_t fromBar, uint8_t countInBeats)
    {
        (void)countInBeats;
        return startRecordingWithSong(fromBar);
    }
    /// End the take (and the song, for a take lined up with one).
    virtual void     stopRecording() = 0;
    /// The take being recorded, or the last one.
    virtual RecStatus recStatus() = 0;
    /// The app is closing: stop metering and put the codec input back.
    virtual void     releaseInput() = 0;

    // ── Files (requests) ──────────────────────────────────────────────────
    /// Request the list of takes; recordings() has it once job() is Ready.
    virtual bool requestRecordings() = 0;
    /// Full paths from the recordings folder, newest name first.
    virtual std::vector<std::string> recordings() = 0;
    /// Columns and envelope of [start, end) of @p file; end 0 = to its end.
    virtual bool requestOverview(const std::string& file, uint32_t start, uint32_t end,
                                 uint16_t columns) = 0;
    /// The last finished overview.
    virtual WaveOverview overview() = 0;
    /// List @p dir -- any folder on the card; "" for the recordings folder.
    virtual bool requestFolder(const std::string& dir) { (void)dir; return false; }
    /// The last finished listing.
    virtual CardFolder folder() { return {}; }
    /// The card's top: a listing never goes above it.
    virtual std::string cardRoot() const { return {}; }
    /// [start, end) of @p file as a new file in the recordings folder, wherever
    /// @p file is; jobResult() is its path.
    virtual bool requestSaveRegion(const std::string& file, uint32_t start, uint32_t end) = 0;
    /// The slices as a kit on the card, in the kit list at once when there is
    /// room; jobResult() is the kit's name, or why it failed.
    virtual bool requestExportKit(const SliceKit& kit) = 0;
    /// The whole of @p file played with @p fx -- transposed (varispeed,
    /// SliceMath::readPitched), then filtered (SampleFilters) -- into @p out,
    /// written beside it and swapped in so nothing ever reads half a file; an
    /// @p out already there is kept. jobResult() is @p out. What a sampler pad
    /// with pitch or filters plays (renderedSamplePath()).
    virtual bool requestRendered(const std::string& file, const std::string& out, const SampleFx& fx)
    {
        (void)file; (void)out; (void)fx;
        return false;
    }
    /// @p file scaled so its loudest sample reaches @p peak (of full scale,
    /// 0..1] into @p out, written beside it and swapped in as a rendered copy
    /// is; an @p out already there is kept. Never turned down: a file within
    /// 0.1 dB of @p peak, or above it, is its own answer and nothing is
    /// written. jobResult() is the file the pad should play -- @p out or
    /// @p file -- or why it failed.
    virtual bool requestNormalized(const std::string& file, const std::string& out, float peak)
    {
        (void)file; (void)out; (void)peak;
        return false;
    }
    /// SNAP: each cut of @p starts (sorted) moved onto the zero crossing
    /// nearest before it in @p file (SliceMath::zeroCrossing), never back onto
    /// the cut before it; snapped() is the result, in the same order.
    virtual bool requestSnapped(const std::string& file, const std::vector<uint32_t>& starts)
    {
        (void)file; (void)starts;
        return false;
    }
    virtual std::vector<uint32_t> snapped() { return {}; }
    /// Where the current file job is.
    virtual ToolJob     job() const = 0;
    /// What the last job came to: a path, a name, or why it failed.
    virtual std::string jobResult() = 0;
    virtual uint8_t     jobProgress() const = 0;   ///< percent

    // ── Preview ───────────────────────────────────────────────────────────
    /// Play [start, end) of @p file (end 0 = to its end), once or looped.
    virtual bool     preview(const std::string& file, uint32_t start, uint32_t end, bool loop) = 0;
    /// preview() shaped as @p sound says -- what a kit made with the same
    /// settings plays. previewFrame() stays in the file's frames whatever the
    /// pitch. A platform without it plays it plain.
    virtual bool     previewAs(const std::string& file, uint32_t start, uint32_t end, bool loop,
                               const PreviewSound& sound)
    {
        (void)sound;
        return preview(file, start, end, loop);
    }
    /// Stop the preview.
    virtual void     stopPreview() = 0;
    /// True while a preview plays.
    virtual bool     previewing() const = 0;
    /// The frame of the file being heard now.
    virtual uint32_t previewFrame() const = 0;

    // ── Recorder to Slicer ────────────────────────────────────────────────
    /// A file and the loop marked in it, for the next screen that asks.
    virtual void setHandoff(const std::string& file, uint32_t start, uint32_t end) = 0;
    /// Takes it: true once per setHandoff().
    virtual bool takeHandoff(std::string& file, uint32_t& start, uint32_t& end) = 0;
};

} // namespace crosspad

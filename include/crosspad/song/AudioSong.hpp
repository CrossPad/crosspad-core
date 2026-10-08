// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace crosspad {

/// One block on an audio track: a WAV from the project's clips/ folder,
/// placed on a bar. It plays once, from its first sample, for as long as the
/// file is -- @c bars is the length it was made to (a bounce knows it), and is
/// what the timeline draws and what "after the last block" is counted from.
struct SongClip {
    std::string file;        ///< name inside <project>/clips/
    uint32_t    bar  = 0;    ///< 0-based start bar
    uint16_t    bars = 1;    ///< length on the grid
};

/// A launcher set: the project's clips arranged in a grid to be played live,
/// four to a row -- one per track -- and a row at a time as a scene. A cell
/// names a clip in the project's clips/ folder, or nothing; a name whose file
/// has gone is kept, so a set says which cell lost its audio instead of
/// quietly closing the gap.
///
/// The set is the project's own and travels in song.json, and a project may
/// hold several: one for a soundcheck, one for the set list. Firmware that
/// predates them ignores the key and drops it the next time it saves that
/// project, which is why nothing else depends on them.
struct ClipSet {
    static constexpr int      kCols    = 4;      ///< == AudioSong::kTracks
    static constexpr uint32_t kMaxRows = 32;

    std::string name;
    /// rows x kCols, row-major. Always a whole number of rows.
    std::vector<std::string> cells;

    uint32_t rows() const { return (uint32_t)(cells.size() / kCols); }
    bool inside(uint32_t row, int col) const
    { return col >= 0 && col < kCols && row < rows(); }
    /// The clip in a cell, or "" -- also for a cell outside the set, so a
    /// screen drawing one row past the end needs no special case.
    const std::string& at(uint32_t row, int col) const;
    void put(uint32_t row, int col, const std::string& file);
    /// Grow with empty rows or drop whole rows from the end, up to kMaxRows.
    void setRows(uint32_t n);
    /// No cell names a clip.
    bool blank() const;
};

/// A set from @p files, four to a row in the order given: what a project that
/// has never had one starts with.
ClipSet clipSetFromFiles(const std::vector<std::string>& files, const std::string& name);

/// What AudioSong::load() found. Unreadable (corrupt, or saved by newer
/// firmware) is kept apart from New so nobody saves an empty song over it.
enum class SongLoad : uint8_t { New, Loaded, Unreadable };

/// What a track holds.
///
/// @c Audio is clips of WAV from the project's clips/ folder. @c Seq is the
/// pattern sequencer's scene, played live with the song -- no file, so it can
/// still be changed note by note while the arrangement grows. One track at a
/// time may be a seq (@c kMaxSeqTracks), and a bounce turns it into audio,
/// which is how that slot is freed for the next idea.
enum class TrackKind : uint8_t { Audio = 0, Seq = 1 };

/// What a seq track's notes play through: the Sampler's kit (sixteen pads,
/// one sample each) or Pitch Play (one sample across the pads). EDIT in the
/// seq view opens that app, so a track carries which one it belongs to.
enum class SeqSource : uint8_t { Kit = 0, Pitch = 1 };

struct SongTrack {
    std::vector<SongClip> clips;   ///< kept sorted by bar
    float volume = 1.0f;
    bool  muted  = false;
    TrackKind kind = TrackKind::Audio;
    SeqSource source = SeqSource::Kit;   ///< only read when kind == Seq
    uint8_t   scene = 0;                 ///< the sequencer scene a Seq track plays
    /// A track the player has made. All four exist in this struct from the
    /// start; this is what tells an empty track waiting for a recording from
    /// one nobody has asked for, which is what "2 / 4 tracks" counts and what
    /// decides whether Arrange offers another. A song written before tracks
    /// had kinds loads with this set wherever it found clips.
    bool      inUse = false;
};

/// An audio song: four tracks of clips on a bar grid, at one tempo.
///
/// A project is a folder -- <root>/<name>/song.json and <root>/<name>/clips/
/// -- so clips bounced from different kits can sit in one song. The grid is
/// the song's bpm and metre; a clip made at another tempo plays at the speed it
/// was recorded (there is no time-stretch), which is why a bounce records the
/// tempo it was made at and the song adopts the first one it is given.
///
/// Plain data plus load/save: portable, no locking. Whoever plays it takes a
/// copy (see ISongEngine::commit), so editing never races playback.
class AudioSong {
public:
    static constexpr int kTracks = 4;
    static constexpr int kVersion = 1;
    static constexpr float kDefaultBpm = 120.0f;       ///< the grid before a clip sets one
    static constexpr uint8_t kDefaultBeatsPerBar = 4;
    static const char* fileName() { return "song.json"; }
    static const char* clipsDir() { return "clips"; }

    std::string dir;              ///< project folder, no trailing slash
    std::string name;
    float       bpm  = 0.0f;      ///< 0 until the first clip sets it
    uint8_t     beatsPerBar = kDefaultBeatsPerBar;
    bool        loop = true;
    uint32_t    length = 0;       ///< bars the song plays (and loops) over; 0 = to its last block
    bool        syncSeq = false;  ///< playing the song also plays the sequencer, locked to it
    /// The loop region, in bars, [loopStart, loopEnd): with loop on, playback
    /// goes round it instead of the whole song. loopEnd <= loopStart = none.
    uint32_t    loopStart = 0;
    uint32_t    loopEnd = 0;
    bool hasRegion() const { return loopEnd > loopStart; }
    SongTrack   tracks[kTracks];
    /// The launcher's sets, and which of them is on screen. Empty on a
    /// project that has never had one.
    std::vector<ClipSet> sets;
    uint8_t     currentSet = 0;

    /// The set the launcher plays from, or nullptr when the project has none.
    ClipSet* currentClipSet();
    const ClipSet* currentClipSet() const;
    /// Append @p set and make it current. Returns its index, or -1 when the
    /// project already holds kMaxSets.
    int addClipSet(const ClipSet& set);
    /// Drop set @p index; the one before it becomes current.
    void removeClipSet(int index);
    static constexpr int kMaxSets = 8;

    SongLoad load(const std::string& projectDir);
    /// Written beside song.json and swapped in, the old file kept as .bak
    /// until the new one is in place; load() falls back to either, so a cut
    /// at any point costs at most the edit being saved.
    bool save() const;

    std::string clipPath(const std::string& file) const { return dir + "/" + clipsDir() + "/" + file; }

    /// The bar after a track's last block, where "append" puts the next one.
    uint32_t trackEnd(int track) const;
    /// The bar after the last block on any track.
    uint32_t songEnd() const;
    /// Where playback ends or loops: the set length, or the last block's end.
    uint32_t playEnd() const { return length ? length : songEnd(); }
    /// Add a block, keeping the track sorted. Returns the index of the block
    /// added, also when an identical one sits on the same bar.
    int addClip(int track, const SongClip& clip);
    void sortTrack(int track);

    /// Samples in one bar at the song's tempo, at @p sampleRate.
    uint32_t samplesPerBar(uint32_t sampleRate) const;

    // ── tracks a player makes ──────────────────────────────────────────────
    /// Tracks that may hold a seq at once. The sequencer plays one scene, so
    /// a second seq track would have nothing of its own to play.
    static constexpr int kMaxSeqTracks = 1;

    /// Tracks in use: what Arrange counts, and what decides whether it still
    /// offers "+ ADD TRACK".
    int usedTracks() const;
    /// Tracks that hold a seq -- 0 or 1.
    int seqTracks() const;
    bool seqSlotFree() const { return seqTracks() < kMaxSeqTracks; }
    /// The lowest track nobody has made yet, or -1 when all four are in use.
    int freeTrack() const;

    /// Make @p track an empty audio track, in use and waiting for a clip.
    /// Returns false for a track already in use.
    bool addAudioTrack(int track);
    /// Make @p track a seq on @p source playing @p scene. Returns false when
    /// the track is in use or the one seq slot is taken.
    bool addSeqTrack(int track, SeqSource source, uint8_t scene);
    /// Turn a seq track into an audio track: what BOUNCE does once the WAV is
    /// written. @p clip is the block the bounce made and becomes the track's
    /// only content; pass nullptr when whoever wrote the WAV has already
    /// placed its block on this track, as ISongEngine::bounceScene() does.
    /// Returns false unless @p track holds a seq. The seq slot is free
    /// afterwards.
    bool bounceSeqTrack(int track, const SongClip* clip = nullptr);
    /// Give @p track back: its clips go, and Arrange stops counting it.
    void removeTrack(int track);
};

} // namespace crosspad

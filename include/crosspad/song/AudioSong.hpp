// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/sampletools/RecInput.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace crosspad {

/// One note of a seq block: where in the block's pattern it is struck and how
/// long it is held, in ticks (AudioSong::kSeqPpq to the quarter) -- so a song
/// keeps its notes through a change of tempo or sample rate, as
/// sequences.json does. @c len 0 is a hit; @c perf is what it sounded as
/// (PadPerformance::pack(), 0 plain), as a scene's event keeps it.
struct SeqNote {
    uint32_t tick = 0;
    uint32_t len  = 0;
    uint8_t  pad  = 0;
    uint8_t  vel  = 0;
    uint8_t  perf = 0;
};

/// A seq block's own notes: a pattern @c bars long, sorted by tick.
///
/// Never changed once made, only replaced. Every copy of a block -- a DUP, a
/// cut's two halves, a region pasted -- and every undo step holds the same
/// one, and an edit (a take dubbed in, the seq view's changes) puts a new one
/// on the block it was made on. That is what makes a copy a clip of its own
/// from the moment either is edited, and costs nothing until then.
struct SeqPattern {
    uint16_t bars = 1;
    std::vector<SeqNote> notes;
};
using SeqPatternPtr = std::shared_ptr<const SeqPattern>;

/// @p notes as a pattern @p bars long, sorted by tick.
SeqPatternPtr makeSeqPattern(uint16_t bars, std::vector<SeqNote> notes);

/// One block on a track, placed on a bar.
///
/// On an audio track it is a WAV from the project's clips/ folder. It plays
/// once, from @c offset, for as long as the file is or for @c frames -- @c bars
/// is the length it was made to (a bounce knows it), and is what the timeline
/// draws and what "after the last block" is counted from. A cut is the one
/// thing that sets @c offset and @c frames: its second half plays on from the
/// cut, its first stops there.
///
/// On a seq track it is notes: its own @c notes (what a take in Arrange
/// makes), or -- a block from before blocks had notes of their own -- the
/// track's scene. Either is played from its bar @c from, looped, for @c bars
/// bars. That is what lets a region copy or a cut move the notes by bars the
/// way it moves the audio.
///
/// On either kind of track it can instead be an element: another arrangement,
/// kept as a project of its own in the library, <songsRoot>/.elements/, and
/// played from a bake of it (a WAV in <songsRoot>/.bake/). Then @c element names it, @c file is the
/// bake's name (empty until it has been baked), @c frames one pass of the
/// element at the bake's tempo (0 until baked), @c bakeBpm that tempo and
/// @c from the element bar the block starts on; @c bar and @c bars are as for
/// any block. A bake made at another tempo than the song's plays varispeed
/// until the song's own is made (varispeed_frames(), BakedView.hpp).
struct SongClip {
    std::string file;        ///< name inside <project>/clips/; empty on a seq track
    uint32_t    bar  = 0;    ///< 0-based start bar
    uint16_t    bars = 1;    ///< length on the grid
    uint32_t    offset = 0;  ///< audio: frames into the file the block starts at
    uint32_t    frames = 0;  ///< audio: frames it plays from @c offset; 0 = to the file's end
    uint16_t    from = 0;    ///< seq: the bar of its notes the block starts on
    SeqPatternPtr notes;     ///< seq: the block's own notes; null = the track's scene
    std::string element;     ///< element ref (.elements/E%08X); empty on every other block
    float       bakeBpm = 0; ///< element: the tempo @c file was baked at; 0 = the song's (linked before bakes kept it)

    bool isElement() const { return !element.empty(); }
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
    static constexpr int      kCols    = 4;      ///< the first kCols of AudioSong::kTracks
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

/// A block copied out of a range of bars: its track, where it starts counted
/// from the range's first bar, and the part of it that sounded there.
struct RangeClip {
    int      track = 0;
    uint32_t rel   = 0;
    SongClip clip;
};

/// What AudioSong::load() found. Unreadable (corrupt, or saved by newer
/// firmware) is kept apart from New so nobody saves an empty song over it.
enum class SongLoad : uint8_t { New, Loaded, Unreadable };

/// What a track holds.
///
/// @c Audio is clips of WAV from the project's clips/ folder. @c Seq is
/// notes, played through the pads with the song -- no file, so they can still
/// be changed note by note while the arrangement grows. Recorded in Arrange,
/// each block holds its own (SongClip::notes). A track from before that plays
/// the pattern sequencer's scene: with no blocks over the whole song, with
/// blocks (seqBlocksExplicit(), then cut and copied by bars) only under them.
/// Three tracks may be seqs at once, one per engine -- two kits and a Pitch
/// track (AudioSong::seqRoute()); a bounce turns one into
/// audio, which is how a slot is freed for the next idea.
enum class TrackKind : uint8_t { Audio = 0, Seq = 1 };

/// What a seq track's notes play through: the Sampler's kit (sixteen pads,
/// one sample each) or Pitch Play (one sample across the pads). EDIT in the
/// seq view opens that app, so a track carries which one it belongs to.
enum class SeqSource : uint8_t { Kit = 0, Pitch = 1 };

/// How an audio track follows a change of the song's tempo (SongTrack::stretch).
enum class StretchMode : uint8_t {
    Off = 0,         ///< a block keeps its speed and its bars follow it, as always
    KeepPitch = 1,   ///< time-stretched: the length changes, the pitch does not
    Repitch = 2,     ///< varispeed, as a turntable: slower is lower, faster is higher
};

struct SongTrack {
    std::vector<SongClip> clips;   ///< kept sorted by bar
    std::string name;              ///< the player's; empty until one is given
    float volume = 1.0f;
    /// -1 left .. +1 right, a balance as the Mixer's pan: the side it moves
    /// away from is turned down. 0 = centre.
    float pan    = 0.0f;
    bool  muted  = false;
    /// Soloed: while any track is, only the soloed ones sound (silenced()).
    bool  solo   = false;
    TrackKind kind = TrackKind::Audio;
    SeqSource source = SeqSource::Kit;   ///< only read when kind == Seq
    uint8_t   scene = 0;                 ///< the sequencer scene a Seq track plays
    /// A Seq track that plays only under its blocks (seqBlocksExplicit()).
    /// Without, it has none and plays its scene over the whole song.
    bool      seqBlocks = false;
    /// An audio track fitted to the song's tempo: each block keeps its bars
    /// when the tempo changes and plays a copy of its audio that fills them,
    /// stretched or repitched (SongEngine renders it while stopped; until then
    /// the block plays as recorded). Off, a block keeps its speed, as always.
    StretchMode stretch = StretchMode::Off;
    /// An audio track's input: what a REC in Arrange records into it while
    /// it is armed (ArrangeAudioTake).
    RecInput  input = RecInput::Mic;
    /// A Kit seq track's own kit (a kit.json as the kit list names it), played
    /// through the second kit engine. Empty, the track plays the song's kit
    /// (AudioSong::kit) through the pads' -- which the first kit track does.
    std::string kit;
    /// A seq track frozen: what it sounded as, as audio blocks over files in
    /// clips/ -- each with its bar, bars and @c offset into its file, so one
    /// pass of a looping block rendered once plays as many times as the block
    /// goes round (empty: never frozen). Its notes stay -- it is still a
    /// Drums, Slice or Pitch track -- and while it is not armed and
    /// @c freezeKey still names its notes, sound and tempo (SongEngine works
    /// the key out), the song plays these instead of its engine.
    std::vector<SongClip> freeze;
    uint32_t  freezeKey = 0;
    /// Moved when what the track plays through changes under the same name
    /// (an instrument saved from edits): a freeze's renders are found on the
    /// card by what they play, sound included, and this is the sound's part.
    uint32_t  freezeSalt = 0;
    /// What the freeze was rendered through, as the card had it then: the
    /// kit's or the instrument's identity (sound_identity()). A kit saved under
    /// its name since -- in this boot or another -- reads otherwise, and the
    /// freeze is of the old sound. 0: not known (a freeze made before it was kept).
    uint32_t  freezeKit = 0;
    /// A track the player has made. All four exist in this struct from the
    /// start; this is what tells an empty track waiting for a recording from
    /// one nobody has asked for, which is what "2 / 4 tracks" counts and what
    /// decides whether Arrange offers another. A song written before tracks
    /// had kinds loads with this set wherever it found clips.
    bool      inUse = false;
};

/// An audio song: up to kTracks tracks of clips on a bar grid, at one tempo.
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
    static constexpr int kTracks = 8;
    /// What firmware before version 6 holds: a song using more is written as 6.
    static constexpr int kTracksV5 = 4;
    /// 2: seq blocks carry notes of their own. 3: two seq tracks, a track
    /// with a kit of its own. 4: a Pitch track beside kit tracks. 5: element
    /// clips (SongClip::element, ::bakeBpm) and a title. 6: more than kTracksV5 tracks. Each written
    /// only by a song that needs it, so the rest stay readable by firmware from
    /// before -- which refuses a newer one rather than open it and drop what it
    /// cannot hold on its next save.
    static constexpr int kVersion = 6;
    /// Ticks to the quarter of a seq block's notes: PatternSequencer::kPpq.
    static constexpr uint16_t kSeqPpq = 96;
    /// The most one seq block a take lays holds: what one scene of the
    /// sequencer holds (PatternSequencer::kMaxBars, ::kMaxEvents), so the seq
    /// view opens every block a REC or a DUB made.
    static constexpr uint16_t kSeqBlockBars  = 8;
    static constexpr size_t   kSeqBlockNotes = 192;
    static constexpr float kDefaultBpm = 120.0f;       ///< the grid before a clip sets one
    static constexpr uint8_t kDefaultBeatsPerBar = 4;
    static const char* fileName() { return "song.json"; }
    /// An element's song.json as it was before an edit not decided yet
    /// (copy or replace), kept beside it: what every other song plays meanwhile.
    static constexpr const char* snapshotFileName() { return "song.orig.json"; }
    static const char* clipsDir() { return "clips"; }
    /// Where the elements live, one project folder each, under the songs
    /// root beside the songs (the library every song shares); and where their
    /// bakes are, next to them.
    static const char* elementsDir() { return ".elements"; }
    static const char* bakeDir() { return ".bake"; }

    std::string dir;              ///< project folder, no trailing slash
    std::string name;
    /// An element's display name; empty on a song, which is named by its folder.
    std::string title;
    /// An element's own latest bake, a file of <songsRoot>/.bake/: the cache
    /// never evicts it, so the element opened again plays at once. Set where
    /// the bake becomes known; empty on a song and on an element not baked yet.
    /// Not part of the content hash.
    std::string newestBake;
    /// The folder is an element of the library (<songsRoot>/.elements/E%08X), titled or not.
    bool inElementLibrary() const;
    float       bpm  = 0.0f;      ///< 0 until the first clip sets it
    uint8_t     beatsPerBar = kDefaultBeatsPerBar;
    bool        loop = true;
    uint32_t    length = 0;       ///< bars the song plays (and loops) over; 0 = to its last block
    bool        syncSeq = false;  ///< playing the song also plays the sequencer, locked to it
    /// The sounds the song is made with, as a DAW's project keeps its
    /// instruments: opening the song loads them, and a kit or instrument
    /// loaded while it is open is written here. @c kit is the kit's kit.json
    /// as the kit list names it (/crosspad/kits/<folder>/kit.json); empty, the
    /// song has none and opening it leaves no kit loaded. @c instrument is
    /// Pitch Play's for the song's Pitch track: an instrument.json from the
    /// card's root (/crosspad/pitchplay/<name>/instrument.json), or one inside
    /// the song's own folder (pitch/instrument.json, no leading '/') -- the
    /// copy the song keeps of one that was never saved. Not part of undo: they
    /// say what is loaded, not how the song is arranged.
    std::string kit;
    std::string instrument;
    /// What the Pitch track's pads play through @c instrument: Pitch Play's
    /// key, scale, octave, transpose and chords, and the notes they put on
    /// each pad. The notes are what a take of pad numbers is played as, so
    /// they belong to the song: without them a song opened after a restart
    /// played its bass at Pitch Play's default octave (and froze it so).
    /// Unset until Pitch Play has published a map with the song open.
    struct PitchPads {
        bool     set = false;
        uint8_t  root = 0;            ///< pitch class of the tonic, 0..11
        int8_t   octave = 1;          ///< PlaySettings::octave
        uint8_t  scale = 0;           ///< MusicTheory scale index
        int8_t   transpose = 0;       ///< semitones on top of all of it
        bool     chords = false;      ///< the pads play the chord list
        uint8_t  note[16] = {};       ///< a pad's own note
        uint8_t  chord[16][6] = {};   ///< a pad's chord notes, 0xFF unused
        uint8_t  count[16] = {};      ///< how many of them
        uint16_t silent = 0;          ///< pads that play nothing, a bit each
        bool operator==(const PitchPads& o) const;
        bool operator!=(const PitchPads& o) const { return !(*this == o); }
    };
    PitchPads pitchPads;
    /// The track REC takes onto (Arrange's ARM): a seq track's pads, whose
    /// kit the pads then play in Arrange, or an audio track's input
    /// (armedAudioTrack()). For the seq takes -1, or a track that is no seq,
    /// is the first seq track (armedSeqTrack()). Not part of undo, as the
    /// sounds are not.
    int8_t      armed = -1;
    /// The kit tracks with a kit of their own that the two kit engines play
    /// now (seqRoute() 1; slot 0 is PlatformServices::secondKit on route 1,
    /// slot 1 ::thirdKit on route 3); -1 for none. Any number of those tracks
    /// are in a song: the others play their freeze (SongTrack::freeze) and are
    /// silent while they have none that is current (liveRoute()). Two, so ARM
    /// can move between them while the song plays: the armed track takes one
    /// at once, the one it left keeps the other until it is frozen
    /// (crosspad::kitSlotsNext(), Arrange). Not saved and not part of undo:
    /// load() gives slot 0 to the armed track, else the first one with a kit
    /// of its own.
    static constexpr int kKitSlots = 2;
    int8_t      kitSlots[kKitSlots] = {-1, -1};
    /// The slot @p track holds, or -1.
    int kitSlotOf(int track) const
    {
        for (int s = 0; s < kKitSlots; ++s)
            if (track >= 0 && kitSlots[s] == track) return s;
        return -1;
    }
    /// The route slot @p slot's engine is on: 1, 3.
    static constexpr int slotRoute(int slot) { return slot == 0 ? 1 : 3; }
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

    SongLoad load(const std::string& projectDir) { return load(projectDir, fileName()); }
    /// The project in @p projectDir from its file @p file (snapshotFileName()) instead of song.json.
    SongLoad load(const std::string& projectDir, const char* file);
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
    /// The same for a tempo and metre given: what a tempo that is not the song's own is laid out on.
    static uint32_t samplesPerBar(uint32_t sampleRate, float bpm, uint8_t beatsPerBar);

    // ── tracks a player makes ──────────────────────────────────────────────
    /// Tracks in use: what Arrange counts, and what decides whether it still
    /// offers "+ ADD TRACK".
    int usedTracks() const;
    /// Tracks that hold a seq: one on the pads' kit, one Pitch, and any number
    /// with a kit of their own (kitSlot), each on a scene of its own.
    int seqTracks() const;
    /// The sequencer's scenes (PatternSequencer::kNumSequences): a seq track
    /// plays one of its own, so there are never more seq tracks than this.
    static constexpr int kSeqScenes = 8;
    bool seqSlotFree() const { return seqTracks() < kSeqScenes && freeSeqRoute(SeqSource::Kit) >= 0; }
    /// Which engine seq track @p track plays through, as the sequencer routes
    /// its notes (PatternSequencer::SongSpan::route): 0 the pads' -- the
    /// song's kit --, 1 the second kit engine, for a kit track with a kit of
    /// its own (SongTrack::kit), 2 the Pitch instrument. -1 for no seq.
    int seqRoute(int track) const;
    /// The engine seq track @p track's notes go to now: seqRoute(), but for a
    /// track with a kit of its own the route of the slot it holds -- 1 or 3
    /// (slotRoute()) --, and -1 when it holds none: its notes go nowhere.
    int liveRoute(int track) const;
    /// The engine a render or edit of track @p track plays through: liveRoute(),
    /// else seqRoute() for a kit of its own in no slot (nothing then holds its
    /// kit -- Arrange gives the track being played a slot first).
    int engineRoute(int track) const
    {
        const int live = liveRoute(track);
        return live >= 0 ? live : seqRoute(track);
    }
    /// The route a new seq track on @p source would take, or -1 when there is
    /// no room for it: route 2 for a Pitch track while no other has it; for a
    /// kit track 0 while no track plays the song's kit, else 1.
    int freeSeqRoute(SeqSource source) const;
    /// The seq track playing through route @p route's engine now -- on 1 and
    /// 3 the one holding that kit slot --, or -1.
    int seqTrackOnRoute(int route) const;
    /// The track @c armed names when it is a seq track in use, else the first
    /// seq track, else -1.
    int armedSeqTrack() const;
    /// The track @c armed names when it is an audio track in use, else -1.
    int armedAudioTrack() const;
    /// Some track is soloed.
    bool anySolo() const;
    /// The seq track in use plays Pitch Play's instrument.
    bool hasPitchTrack() const;
    /// Track @p t makes no sound: muted, or another track is soloed and it
    /// is not. What the engine plays by, for audio and seq tracks alike.
    bool silenced(int t) const;
    /// Track @p t is a seq track laid out as blocks with none left -- they went
    /// into an element, or were deleted: nothing of it is heard, so nothing of it
    /// needs a freeze to be mixed from the card.
    bool seqPlaysNothing(int t) const;
    /// The lowest track nobody has made yet, or -1 when all are in use.
    int freeTrack() const;

    /// Make @p track an empty audio track, in use and waiting for a clip.
    /// Returns false for a track already in use.
    bool addAudioTrack(int track);
    /// A seq track in use other than @p except plays @p scene.
    bool sceneUsed(uint8_t scene, int except = -1) const;
    /// The scene a new seq track should take, among @p sceneCount: the one
    /// just added (the last, when @p justAdded) -- **not** the sequencer's
    /// active scene, which while it plays is still the old one until the loop
    /// ends, and gave a Slice track the Drums track's scene (22.10.2026) --
    /// else the lowest no seq track plays. -1 when every one is taken.
    int sceneForNewSeq(uint8_t sceneCount, bool justAdded) const;
    /// Make @p track a seq on @p source playing @p scene. A kit track takes
    /// the route that is free (freeSeqRoute()): on 0 it plays the song's kit,
    /// which @p kit, given, becomes; on 1 @p kit is its own and must be
    /// given, and it takes a kit slot no track holds. Every new seq track is empty and plays only under its blocks
    /// (SongTrack::seqBlocks). Returns false when the track is
    /// in use, there is no room for the seq, or another seq track plays
    /// @p scene -- two tracks on one scene overwrite each other's notes.
    bool addSeqTrack(int track, SeqSource source, uint8_t scene, const std::string& kit = {});
    /// Turn a seq track into an audio track: what BOUNCE does once the WAV is
    /// written. @p clip is the block the bounce made and becomes the track's
    /// only content; pass nullptr when whoever wrote the WAV has already
    /// placed its block on this track, as ISongEngine::bounceScene() does.
    /// Returns false unless @p track holds a seq. The seq slot is free
    /// afterwards.
    bool bounceSeqTrack(int track, const SongClip* clip = nullptr);
    /// Give @p track back: its clips go, and Arrange stops counting it.
    void removeTrack(int track);

    // ── editing by bars ────────────────────────────────────────────────────
    //
    // A block sounds from its bar to its end or to the next block's bar,
    // whichever comes first (SongEngine's flatten()); these work on what
    // sounds. @p spBar -- samples a bar, samplesPerBar() at the engine's rate
    // (ISongEngine::framesPerBar()) -- turns bars into an audio block's
    // offset and length.

    /// Cut the block of @p track that sounds across @p bar in two there: the
    /// first keeps what is before the cut, the second -- a new block on
    /// @p bar -- plays on from it. Returns the second's index, or -1 when no
    /// block sounds across @p bar (a block's own first bar is not inside it).
    int cutClip(int track, uint32_t bar, uint32_t spBar);
    /// Make room at @p at: every block sounding across it is cut there, and
    /// every block from it on moves @p by bars later, as does the set length.
    /// Nothing after the point is written over.
    void insertBars(uint32_t at, uint32_t by, uint32_t spBar);
    /// Take [@p from, @p to) out of the song and close the gap: what sounds
    /// in it goes (clearRange()), every block after it moves that many bars
    /// earlier, as do the set length and a loop region after or around it.
    void deleteBars(uint32_t from, uint32_t to, uint32_t spBar);
    /// What sounds in [@p from, @p to) on every track, by bars: a block
    /// reaching in from before is copied from @p from, one reaching past is
    /// copied up to @p to. Seq blocks as much as audio ones.
    std::vector<RangeClip> copyRange(uint32_t from, uint32_t to, uint32_t spBar) const;
    /// Nothing sounds in [@p from, @p to) on any track: a block reaching in from
    /// before is cut at @p from, one reaching past plays on from @p to, and
    /// the blocks starting inside go.
    void clearRange(uint32_t from, uint32_t to, uint32_t spBar);
    /// @p clips back in, the range's first bar on @p at. Over what is there:
    /// insertBars() first to keep it. A block whose track is no longer of
    /// its kind is left out.
    void pasteRange(const std::vector<RangeClip>& clips, uint32_t at);
    /// A seq track without seqBlocks plays its scene over the whole song;
    /// this gives it that as one block of @p bars from bar 0 and sets
    /// seqBlocks, so it can be cut and copied. True when a track changed.
    bool seqBlocksExplicit(uint32_t bars);

    // ── takes onto a seq track (Arrange's REC and DUB) ─────────────────────

    /// Ticks in one bar of the song's metre.
    uint32_t seqTicksPerBar() const { return (uint32_t)kSeqPpq * beatsPerBar; }
    /// Every block of seq track @p track that plays the track's scene gets
    /// @p scene as notes of its own: what the scene's notes are now, so a
    /// take can be dubbed into the block and a copy of it be its own. A
    /// track playing its scene over the whole song is given that as one
    /// block of @p bars first. Returns the blocks changed.
    int adoptSceneNotes(int track, const SeqPatternPtr& scene, uint32_t bars);
    /// REC: @p pattern as one block on @p bar of seq track @p track, as long
    /// as the pattern -- or @p bars long, the pattern going round in it. The
    /// seq blocks that sounded there go -- one reaching in from before is cut
    /// at @p bar, one reaching past the end plays on from there. Returns the
    /// new block's index, -1 when @p track is not a seq track in use.
    int putSeqTake(int track, uint32_t bar, const SeqPatternPtr& pattern, uint32_t spBar, uint32_t bars = 0);
    /// REC: @p notes -- ticks from @p bar -- over @p bars bars, laid with
    /// putSeqTake() as blocks the seq view opens: one per kSeqBlockBars bars
    /// from @p bar, halved while more than kSeqBlockNotes notes fall in it. A
    /// note held over the line between two blocks ends on it. Returns the
    /// first block's index, -1 as putSeqTake().
    int putSeqTakeNotes(int track, uint32_t bar, uint32_t bars, std::vector<SeqNote> notes, uint32_t spBar);
    /// DUB: @p notes -- ticks from the song's bar one -- into the blocks of
    /// seq track @p track that sound where each falls. Such a block gets a
    /// new pattern with them added (its copies keep theirs); a block that
    /// plays the scene is left alone. Notes under no block become blocks of
    /// their own: whole bars from the first such note's to the last's, never
    /// over the next block, cut as putSeqTakeNotes() cuts a take. False when
    /// @p track is not a seq track in use.
    bool dubSeqNotes(int track, const std::vector<SeqNote>& notes);

private:
    uint32_t soundingEnd(int track, int index) const;
    /// @p c from @p skip bars into it, for @p bars bars.
    SongClip part(int track, const SongClip& c, uint32_t skip, uint32_t bars, uint32_t spBar) const;
    void dropSeqBlocks(int track);
};

/// @p name is one plain path component -- not empty, not "." or "..", no
/// '/' or '\\' -- which is all a clip's file may be: songs come from other
/// computers, and a name that climbs out of clips/ would be read or written
/// wherever it points. load() empties a name that is not one.
bool is_clip_file_name(const std::string& name);

/// @p name is a file a track's block may play: a clip file name, or one
/// inside clips/.freeze/ -- where a bounced seq track's audio stays, as its
/// freeze rendered it.
bool is_song_clip_name(const std::string& name);

/// The folder name of an element made from @p seed: "E" and eight upper-case
/// hex digits.
std::string make_element_id(uint32_t seed);

/// @p ref is exactly ".elements/E" and eight upper-case hex digits -- what
/// SongClip::element holds, and the only shape a path built from it may take.
bool is_element_ref(const std::string& ref);

} // namespace crosspad

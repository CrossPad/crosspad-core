// SPDX-License-Identifier: MIT
#pragma once

#include "crosspad/song/AudioSong.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace crosspad {

/// Where a bounce is.
enum class BounceState : uint8_t {
    Idle,       ///< nothing running; the last result stays readable
    Running,    ///< the scene is playing and being written
    Done,       ///< written and placed on its track
    Failed,     ///< see BounceStatus::error
};

/// Where opening a project is.
enum class SongOpState : uint8_t {
    Idle,       ///< nothing asked for yet
    Busy,       ///< the platform is reading the card
    Ready,      ///< song() is the project asked for (new or loaded)
    Failed,     ///< the song on the card could not be read; it is never saved over
};

/// What the sequencer does when a song playing in sync starts it: play its
/// scene, or open a take -- so REC pressed with the song stopped starts both,
/// and the take lands on the song's bars.
enum class SyncTake : uint8_t { None, Record, Overdub };

/// A clip file in the open project, with its length on the song's grid.
struct ClipInfo {
    std::string file;
    uint16_t    bars = 1;
};

/// A clip's waveform, for drawing it: the loudest sample of each run of
/// @c hop frames, either channel, 0..255 of full scale. Empty when the file
/// is not a WAV the engine plays.
struct ClipPeaks {
    uint32_t hop = 0;
    std::shared_ptr<const std::vector<uint8_t>> values;
    /// The clip's file is not in the project's clips folder. A song keeps the
    /// name of what it plays, and a folder that has been through another
    /// computer comes back without some of it -- so this is ordinary, not an
    /// error, and a screen must be able to say which block has nothing behind
    /// it instead of drawing it as silence.
    bool missing = false;
};

struct BounceStatus {
    BounceState state = BounceState::Idle;
    uint32_t    framesDone  = 0;
    uint32_t    framesTotal = 0;
    std::string file;          ///< clip name inside the project's clips/
    std::string error;         ///< why it failed; with Done, a warning (tempo off the grid)
};

/// The audio song side of the platform: projects on the card, bouncing a
/// sequencer scene into one of them, and playing the four tracks back.
///
/// Apps reach it through PlatformServices::songEngine and never see how the
/// platform streams or records -- card and task work only a platform can do.
///
/// Nothing here waits for the card. Every call that needs it is a request
/// that returns at once; the result shows up in openState(), projects(),
/// clips() and listGeneration(), which an app polls from its UI timer. A card
/// busy with a kit load or a bounce would otherwise freeze the UI thread that
/// asked.
///
/// The song model itself (AudioSong) is portable data: edit song() holding
/// songMutex() -- a finished bounce adds its block from the platform's own
/// thread -- then commit() hands the arrangement to playback and queues the
/// save.
class ISongEngine {
public:
    virtual ~ISongEngine() = default;

    // ── Projects ──────────────────────────────────────────────────────────
    /// Refresh projects(). False when the request could not be queued.
    virtual bool requestProjects() = 0;
    /// Project names on the card, sorted, as of the last finished refresh.
    virtual std::vector<std::string> projects() = 0;
    /// Open @p name, creating it when it does not exist. The open project is
    /// remembered across boots.
    virtual bool requestOpen(const std::string& name) = 0;
    virtual SongOpState openState() const = 0;
    /// The open project. Read or edit it holding songMutex().
    virtual AudioSong& song() = 0;
    virtual std::recursive_mutex& songMutex() = 0;
    /// Give playback the edited arrangement at once, and have it saved.
    virtual void commit() = 0;
    /// Refresh clips() from the open project's clips/ folder.
    virtual bool requestClips() = 0;
    /// The clips as of the last finished refresh, sorted by name.
    virtual std::vector<ClipInfo> clips() = 0;
    /// Moves on every finished projects or clips refresh.
    virtual uint32_t listGeneration() const = 0;
    /// Rename the open project's folder; editState() says how it went
    /// (Failed: the name is taken or not one a folder can have).
    virtual bool requestRename(const std::string& name) { (void)name; return false; }
    /// Delete the open project from the card -- its song and every clip in
    /// it -- and open another: the first left on the card, or a new "Song 1"
    /// when none is. Playback stops first. editState() says when it is done
    /// (Failed: no project open, no card, or a file the card would not let
    /// go of; whatever went stays gone).
    virtual bool requestDelete() { return false; }
    /// Set the open song's tempo. Blocks stay on their bars and each is
    /// resized to what its file lasts at the new tempo (no time-stretch: a
    /// clip keeps its speed). editState() says when it is done.
    virtual bool requestTempo(float bpm) { (void)bpm; return false; }
    virtual SongOpState editState() const { return SongOpState::Idle; }
    /// Mix the tracks in @p trackMask (bit t = track t; muted ones are left
    /// out) from @p fromBar up to @p toBar -- or further, to the end of the
    /// last block that starts inside -- into one new clip, at each track's
    /// volume. The clip lands on the lowest of those tracks at @p fromBar;
    /// the blocks it replaces go, and a block reaching into the range from
    /// before it is cut at @p fromBar. editState() says when it is done,
    /// editProgress() how far it is.
    virtual bool requestMerge(uint8_t trackMask, uint32_t fromBar, uint32_t toBar)
    {
        (void)trackMask; (void)fromBar; (void)toBar;
        return false;
    }
    virtual uint8_t editProgress() const { return 0; }
    /// Write the open song as an Ableton Live 12 Set into its own folder --
    /// <name>.als beside song.json, and the "Ableton Project Info" folder
    /// that makes it a Live project, so the Set finds its clips in clips/
    /// where they are (crosspad/song/LiveSet.hpp). An .als already there,
    /// one Live saved over say, moves to Backup/ first. Playback stops.
    /// editState() says when it is done, editReport() what it wrote.
    virtual bool requestExportLiveSet() { return false; }
    /// The last export, one line each: where the Set is and how many clips
    /// went in, then what was left out; with Failed, why.
    virtual std::vector<std::string> editReport() { return {}; }

    // ── Waveforms ─────────────────────────────────────────────────────────
    /// @p file's waveform, a clip of the open project. False until the
    /// platform has it: asking puts it in line, and it is read from the card
    /// while nothing plays (once -- it is kept beside the song after that).
    /// peaksGeneration() moves each time one arrives.
    virtual bool clipPeaks(const std::string& file, ClipPeaks& out) { (void)file; (void)out; return false; }
    virtual uint32_t peaksGeneration() const { return 0; }
    /// Frames a bar lasts at the open song's tempo: where a clip's frames
    /// fall on the bars it is drawn on.
    virtual uint32_t framesPerBar() { return 0; }
    /// Copy [start, end) of @p file -- a 16-bit WAV at the engine's rate, end
    /// 0 = to its end -- into the open project's clips/ and lay it on
    /// @p track at @p bar, after @p lead frames of silence, as many bars long
    /// as that lasts at the song's tempo. Blocks on the track that start
    /// inside it go, and one reaching in from before is cut at @p bar, as a
    /// merge does. editState() says when it is done, editProgress() how far.
    virtual bool requestImport(const std::string& file, uint32_t start, uint32_t end,
                               uint8_t track, uint32_t bar, uint32_t lead)
    {
        (void)file; (void)start; (void)end; (void)track; (void)bar; (void)lead;
        return false;
    }

    // ── Bounce ────────────────────────────────────────────────────────────
    /// Play @p scene of the sequencer once, from its first bar, and write the
    /// pads to a clip in the open project, placed after the last block of
    /// @p track. Poll bounceStatus().
    virtual bool bounceScene(uint8_t scene, uint8_t track) = 0;
    virtual BounceStatus bounceStatus() = 0;
    virtual void cancelBounce() = 0;

    // ── Live clips (the clip launcher) ────────────────────────────────────
    /// One track of the launcher as it stands. @c file loops now; @c pending
    /// was launched and starts on the next quant line, which is why both are
    /// shown: a lit cell and a blinking one.
    struct LiveTrack {
        std::string file;            ///< clip looping now; empty = silent
        std::string pending;         ///< launched, waiting for the line
        uint16_t    bars = 0;        ///< the loop, on the song's grid
        uint32_t    loopFrames = 0;
        uint32_t    phase = 0;       ///< frames into the loop
        bool        stopping = false;///< a stop is waiting for the line
    };

    /// The launcher's whole state, read from a UI timer.
    struct LiveState {
        bool      on = false;                   ///< the live transport runs
        LiveTrack tracks[AudioSong::kTracks];
        double    bars = 0.0;                   ///< where the grid is
        uint8_t   quantBars = 1;
        uint32_t  framesPerBar = 0;
    };

    /// Start the live transport: a free-running bar grid at the open song's
    /// tempo, with nothing playing until a clip is launched. The arrangement
    /// is not touched -- live and the timeline are two ways of playing one
    /// project, never both at once.
    virtual bool liveStart() { return false; }
    /// Launch @p file (a clip of the open project, @p bars long on the grid)
    /// on @p track, from the next quant line on, looping until something
    /// replaces it. Launches made between two lines all take effect on the
    /// same sample, which is what makes a row land together. Starts the live
    /// transport when it is not running.
    virtual bool liveLaunch(uint8_t track, const std::string& file, uint16_t bars)
    {
        (void)track; (void)file; (void)bars;
        return false;
    }
    /// Stop @p track on the next line; kTracks or more: every track. The
    /// transport keeps running, so the grid stays where it is.
    virtual bool liveStop(uint8_t track) { (void)track; return false; }
    /// Bars a launch waits for: 1, 2 or 4 (0 is taken as 1).
    virtual void liveQuant(uint8_t bars) { (void)bars; }
    /// False on a platform with no live transport.
    virtual bool liveState(LiveState& out) { (void)out; return false; }

    // ── Playback ──────────────────────────────────────────────────────────
    /// Requests too: playing() turns true once the platform has the clips open.
    virtual bool   play(uint32_t fromBar) = 0;
    /// play(), and with the song's syncSeq on the sequencer starts with it
    /// doing @p take. A platform without it plays the song alone.
    virtual bool   playWithTake(uint32_t fromBar, SyncTake take) { (void)take; return play(fromBar); }
    /// playWithTake() after @p beats of count-in at the song's tempo: the
    /// song -- and the sequencer with it, in sync -- starts on the sample
    /// after the last click. In sync the metronome counts; alone, the song
    /// plays the same clicks itself. A platform without it plays at once.
    virtual bool   playCountedIn(uint32_t fromBar, SyncTake take, uint8_t beats)
    {
        (void)beats;
        return playWithTake(fromBar, take);
    }
    /// Beats of count-in still to come, 0 once the song plays or when there
    /// is none: 4, 3, 2, 1 on screen.
    virtual uint8_t countInBeatsLeft() const { return 0; }
    virtual void   stop() = 0;
    virtual bool   playing() const = 0;
    /// Where playback is, in bars (fractional) from the song's start.
    virtual double positionBars() const = 0;
    /// Frames the playback running now has sounded: 0 while stopped, priming
    /// or waiting for the sequencer's clock. Lock-free, for a platform's
    /// render to ask every block -- the first block it is not 0 in is the
    /// block the song started in, which is what a take lines up with.
    virtual uint32_t framesPlayed() const { return 0; }
    /// Blocks of silence played because the card fell behind, since play().
    virtual uint32_t underruns() const = 0;
    /// Clip opens the card refused since play(); each is a track gone quiet.
    virtual uint32_t openFailures() const = 0;
};

} // namespace crosspad

// SPDX-License-Identifier: MIT
#pragma once

#include "crosspad/song/AudioSong.hpp"

#include <cstdint>
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
    /// Set the open song's tempo. Blocks stay on their bars and each is
    /// resized to what its file lasts at the new tempo (no time-stretch: a
    /// clip keeps its speed). editState() says when it is done.
    virtual bool requestTempo(float bpm) { (void)bpm; return false; }
    virtual SongOpState editState() const { return SongOpState::Idle; }

    // ── Bounce ────────────────────────────────────────────────────────────
    /// Play @p scene of the sequencer once, from its first bar, and write the
    /// pads to a clip in the open project, placed after the last block of
    /// @p track. Poll bounceStatus().
    virtual bool bounceScene(uint8_t scene, uint8_t track) = 0;
    virtual BounceStatus bounceStatus() = 0;
    virtual void cancelBounce() = 0;

    // ── Playback ──────────────────────────────────────────────────────────
    /// Requests too: playing() turns true once the platform has the clips open.
    virtual bool   play(uint32_t fromBar) = 0;
    /// play(), and with the song's syncSeq on the sequencer starts with it
    /// doing @p take. A platform without it plays the song alone.
    virtual bool   playWithTake(uint32_t fromBar, SyncTake take) { (void)take; return play(fromBar); }
    virtual void   stop() = 0;
    virtual bool   playing() const = 0;
    /// Where playback is, in bars (fractional) from the song's start.
    virtual double positionBars() const = 0;
    /// Blocks of silence played because the card fell behind, since play().
    virtual uint32_t underruns() const = 0;
    /// Clip opens the card refused since play(); each is a track gone quiet.
    virtual uint32_t openFailures() const = 0;
};

} // namespace crosspad

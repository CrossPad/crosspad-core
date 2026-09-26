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
/// platform streams or records -- which is all card and task work that only a
/// platform can do. The song model itself (AudioSong) is portable data: edit
/// song() holding songMutex() -- a finished bounce adds its block from the
/// platform's own thread -- then commit() hands the arrangement to playback
/// and has it written to the card.
class ISongEngine {
public:
    virtual ~ISongEngine() = default;

    // ── Projects ──────────────────────────────────────────────────────────
    /// Project names on the card, sorted.
    virtual std::vector<std::string> listProjects() = 0;
    /// Open @p name, creating it when it does not exist. The open project is
    /// remembered across boots. Blocks until the card work is done.
    virtual bool openProject(const std::string& name) = 0;
    /// The open project. Read or edit it holding songMutex().
    virtual AudioSong& song() = 0;
    virtual std::recursive_mutex& songMutex() = 0;
    /// Give playback the edited arrangement at once, and have it saved.
    virtual void commit() = 0;
    /// The clips in the open project's clips/ folder, sorted by name.
    virtual std::vector<ClipInfo> listClips() = 0;

    // ── Bounce ────────────────────────────────────────────────────────────
    /// Play @p scene of the sequencer once, from its first bar, and write the
    /// pads to a clip in the open project, placed after the last block of
    /// @p track. Asynchronous: poll bounceStatus().
    virtual bool bounceScene(uint8_t scene, uint8_t track) = 0;
    virtual BounceStatus bounceStatus() = 0;
    virtual void cancelBounce() = 0;

    // ── Playback ──────────────────────────────────────────────────────────
    virtual bool   play(uint32_t fromBar) = 0;
    virtual void   stop() = 0;
    virtual bool   playing() const = 0;
    /// Where playback is, in bars (fractional) from the song's start.
    virtual double positionBars() const = 0;
    /// Blocks of silence played because the card fell behind, since play().
    virtual uint32_t underruns() const = 0;
};

} // namespace crosspad

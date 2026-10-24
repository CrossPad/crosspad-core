// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace crosspad {

/// Where Arrange is among the song's elements: the song, and the elements
/// opened from it one inside the other -- the breadcrumb.
///
/// Every move is a job of phases that poll() walks, requests to the song
/// engine and reads of its state only, so Arrange's LVGL timer can drive it:
/// the card is the engine worker's. Opening an element never saves it as the
/// project to open at boot (ISongEngine::requestOpenElement): a restart
/// inside an element opens the song it was reached from.
///
/// Entering stops the song, opens the element and, when it was made at
/// another tempo, fits it to its parent's: its audio tracks that keep their
/// speed (StretchMode::Off) become time-stretched, so a recording follows the
/// song. Leaving stops, freezes every seq track that is not frozen, waits
/// for the fitted copies of stretched tracks, bakes the element
/// (ISongEngine::requestBakeElement) and opens the parent, where every block
/// of the element takes the bake's file and length. A bake that fails, or
/// anything else, stops the job where it is: a leave that failed leaves the
/// engine on the element and path() as it was.
class ElementNav {
public:
    /// One step of the breadcrumb.
    struct Level {
        std::string open;      ///< what the engine opens: the project's name, or the element's ref
        std::string title;     ///< an element's title; empty on the song and on one not yet named
        int         track = -1;   ///< where the level above holds this element
        uint32_t    bar = 0;
    };
    enum class Job : uint8_t { None, Enter, Leave, Insert };
    /// What poll() has to say about a job that has just ended.
    struct Result {
        bool        done = false;   ///< a job ended since the last poll
        bool        ok = false;
        Job         job = Job::None;
        std::string error;          ///< why it did not, when it did not
    };

    /// [0] is the song, open = its project name; each level after it an element
    /// inside the one before.
    const std::vector<Level>& path() const;
    /// path().size() - 1: 0 at the song.
    int depth() const;
    bool busy() const { return job_ != Job::None; }
    /// 0..100 of the job running: what its freeze or bake has come to.
    uint8_t progress() const;
    /// What the job is doing: "stopping", "opening", "freezing", "baking",
    /// "linking"; empty while none runs.
    const char* stage() const;

    /// Open the element block sounding on @p bar of @p track of the open song.
    /// False, with lastError() saying why, when there is none or a job runs.
    bool enter(int track, uint32_t bar);
    /// One level up, baking what changed. False at the song or with a job running.
    bool leave();
    /// leave() until depth() == @p level.
    bool leaveTo(int level);
    /// A new element of kNewElementBars bars called @p title, laid on an audio
    /// (or a still unused) track of the open song at @p bar and entered. A seq
    /// track is refused.
    bool insertNew(int track, uint32_t bar, const std::string& title);
    /// The element @p ref of the library laid on @p track at @p bar, baked at
    /// the open song's tempo (entered and left). Refused when it is on path() or
    /// holds one that is, and when elements() -- as of its last
    /// requestElements() -- does not list it.
    bool insertExisting(int track, uint32_t bar, const std::string& ref);

    /// Walks the job one step; Arrange's timer calls it every ~50 ms. Cheap
    /// while none runs.
    Result poll();
    /// Another song was opened (from the list): the breadcrumb starts over.
    void reset();
    const std::string& lastError() const { return error_; }

    /// Bars of an element made by insertNew().
    static constexpr uint16_t kNewElementBars = 4;

private:
    enum class Phase : uint8_t { Idle, Stopping, Opening, Retempo, Freezing, Stretching, Baking, Reopening };

    /// The engine the job runs on, or null with lastError() set.
    class ISongEngine* engine();
    /// Level 0 follows the project the engine has open while no job runs.
    void syncRoot() const;
    bool refuse(const std::string& why);
    /// Lay an element block on @p track at @p bar of the open song.
    bool addBlock(int track, uint32_t bar, uint16_t bars, const std::string& ref);
    bool startEnter(const std::string& ref, int track, uint32_t bar, Job job);
    void startLeave(Job job);
    Result fail(const std::string& why);
    Result finish();
    /// A request the engine refused: waits a few polls (it is busy with
    /// another edit), then gives the job up.
    Result refused(const std::string& why);
    Result stopped();
    Result opened();
    Result retempo();
    Result entered();
    Result freezing();
    Result baking();
    Result reopening();
    Result linked();
    /// Seq tracks of the open song a bake would find not frozen, from @c track_ on.
    int nextToFreeze();

    mutable std::vector<Level> path_;
    Job         job_ = Job::None;
    Phase       phase_ = Phase::Idle;
    bool        asked_ = false;       ///< the phase's request was taken; its end is awaited
    uint32_t    refusals_ = 0;
    std::string ref_;                 ///< the element the job enters or leaves
    Level       level_;               ///< the level an enter pushes
    float       parentBpm_ = 0.0f;
    std::string newTitle_;            ///< insertNew's, set once entered
    bool        leaveAfterEnter_ = false;   ///< insertExisting: bake what was just laid
    int         targetDepth_ = 0;     ///< where a leave stops
    int         freezeTrack_ = 0;
    std::string bakeFile_;
    uint32_t    bakeFrames_ = 0;
    std::string error_;
};

/// The board's one element navigation.
ElementNav& elementNav();

} // namespace crosspad

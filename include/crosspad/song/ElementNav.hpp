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
/// One tempo, the song's: taken when the first element is entered, every
/// element under the song is fitted to it and baked at it. Entering stops the
/// song, opens the element and, when it was made at another tempo, fits it to
/// the song's: its audio tracks that keep their speed (StretchMode::Off)
/// become time-stretched, so a recording follows the song. Leaving stops,
/// puts the element back at the song's tempo when it was turned inside it,
/// waits for its kit and instrument to be loaded (PlatformServices::songSounds),
/// freezes every seq track that is not frozen, waits for the fitted copies of
/// stretched tracks, bakes the element (ISongEngine::requestBakeElement) and
/// opens the parent, where every block of the element takes the bake's file
/// and length. A bake that fails, or anything else, stops the job where it
/// is: a leave that failed leaves the engine on the element and path() as it
/// was. Another song opened behind its back stops it too, before it freezes,
/// bakes or links anything in that song; idle, the breadcrumb starts over at
/// the song the engine has open.
class ElementNav {
public:
    /// One step of the breadcrumb.
    struct Level {
        std::string open;      ///< what the engine opens: the project's name, or the element's ref
        std::string title;     ///< an element's title; empty on the song and on one not yet named
        int         track = -1;   ///< where the level above holds this element
        uint32_t    bar = 0;
    };
    enum class Job : uint8_t { None, Enter, Leave, Insert, Make };
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
    /// What the job is doing: "stopping", "opening", "fitting" (back to the
    /// song's tempo), "loading" (the kit and instrument), "freezing",
    /// "baking", "linking", "copying" (an element being made); empty while
    /// none runs.
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

    /// A new element made of [@p fromBar, @p toBar) of the open song
    /// (ISongEngine::requestElementFromRegion), the region cleared on every
    /// track and one block of it laid on the lowest audio track the region
    /// touched -- else the lowest free one --, then entered and left, which
    /// bakes it and links the block. Refused ("needs an audio or free track")
    /// when there is neither. The song is not touched until the element is
    /// made; one that fails leaves it as it was. The element is called
    /// @p title (empty: it stays unnamed) and keeps the region's length.
    bool makeFromRegion(uint32_t fromBar, uint32_t toBar, const std::string& title = {});
    /// The project @p songName copied into the library as a new element
    /// (ISongEngine::requestElementFromSong) and laid as insertExisting() lays
    /// one. The copy is the element: a song renamed or edited later changes
    /// nothing in it, and the reference stays an id.
    bool insertSong(int track, uint32_t bar, const std::string& songName);

    /// Walks the job one step; Arrange's timer calls it every ~50 ms. Cheap
    /// while none runs.
    Result poll();
    /// Another song was opened (from the list): the breadcrumb starts over.
    /// One opened anywhere else is noticed by path() on its own.
    void reset();
    const std::string& lastError() const { return error_; }

    /// Bars of an element made by insertNew().
    static constexpr uint16_t kNewElementBars = 4;

private:
    enum class Phase : uint8_t {
        Idle, Making, Listing, Stopping, Opening, Retempo, Refit, Sounds, Freezing, Stretching, Baking, Reopening
    };

    /// The engine the job runs on, or null with lastError() set.
    class ISongEngine* engine();
    /// While no job runs, level 0 follows the project the engine has open,
    /// and a path whose last level is not what it has open starts over there.
    void syncRoot() const;
    /// The engine has @p open open: nothing opened it something else since.
    bool onLevel(const std::string& open);
    bool refuse(const std::string& why);
    /// Lay an element block on @p track at @p bar of the open song.
    bool addBlock(int track, uint32_t bar, uint16_t bars, const std::string& ref);
    /// insertExisting() with the checks on the job and the engine done.
    bool placeExisting(int track, uint32_t bar, const std::string& ref);
    /// An element being made: waits for the engine's copy, then lays it.
    void startMaking(Job job);
    Result making();
    /// The library read again after a song was copied into it.
    Result listing();
    Result laidFromRegion();
    /// fail() for a job whose element is already made: it is discarded.
    Result failMade(const std::string& why);
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
    /// The level being left is on its way up: its tempo first.
    Result toLeaving();
    Result refit();
    Result toSounds();
    Result sounds();
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
    float       songBpm_ = 0.0f;      ///< the song's tempo, taken as the first level is entered
    std::string newTitle_;            ///< insertNew's, set once entered
    bool        leaveAfterEnter_ = false;   ///< insertExisting: bake what was just laid
    int         targetDepth_ = 0;     ///< where a leave stops
    int         track_ = 0;           ///< where a made element's block goes
    uint32_t    bar_ = 0;
    uint32_t    regionBars_ = 0;
    int         freezeTrack_ = 0;
    uint32_t    lastWaiting_ = 0;     ///< stretched blocks still unfitted at the last poll
    std::string bakeFile_;
    uint32_t    bakeFrames_ = 0;
    std::string error_;
};

/// The board's one element navigation.
ElementNav& elementNav();

} // namespace crosspad

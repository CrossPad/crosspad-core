// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/kit/KitInfo.hpp"
#include "crosspad/song/AudioSong.hpp"
#include "crosspad/util/RetryBackoff.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace crosspad {

/// What SongSounds needs from the platform it runs on.
class ISongSoundsHost {
public:
    virtual ~ISongSoundsHost() = default;

    /// The card's root on the file system ("/sdcard" on the board). A song's
    /// folder (AudioSong::dir) carries it; kit and instrument paths do not.
    virtual std::string cardRoot() = 0;
    /// The kit list is read and no kit load is running.
    virtual bool kitsSettled() = 0;
    /// Load kit @p kitId of the kit manager, on the kit loader's worker.
    virtual void loadKit(int kitId) = 0;
    /// No kit current, and the kits' engine as a boot without one leaves it.
    virtual void unloadKit() = 0;
    /// An app holds the pitched engine through IInstrumentHost.
    virtual bool instrumentOwnsEngine() = 0;
    /// The name of the app on screen; nullptr for none.
    virtual const char* foregroundApp() = 0;
    /// Tell the player (a toast).
    virtual void warn(const char* msg) = 0;
    virtual void log(bool warning, const char* msg) = 0;
};

/**
 * @brief The open song's sounds, as a DAW's project keeps its instruments.
 *
 * AudioSong::kit and ::instrument name what a song is made with. Opening a
 * song loads them: its kit -- or, a song with none, no kit at all, the
 * memory given back -- and the Pitch instrument when it has a Pitch track.
 * A kit or an instrument loaded while a song is open is written into it, so
 * the song says what was played the next time it opens.
 *
 * Opening a song is a request the song engine's worker carries out; this
 * hears of it (songOpened()) and brings the board to the song on the UI
 * thread a step at a time (step(), from a timer every kStepMs). A step waits
 * for whatever is already moving -- the kit scan at boot, a kit load, the
 * instrument host -- rather than queueing behind it, because each of those
 * changes what the next step sees: an instrument given back reloads the kit,
 * for one, so the kit is compared only once that has happened.
 *
 * The song's kit is followed after it settles too: a kit given to the song
 * later (Arrange's kit track) is loaded as an opening song's is, and a kit
 * load that finishes after the song was given a newer one is not written
 * over it -- the newer pick wins. The song's kit is its live kit track's
 * (AudioSong::liveKit): Arrange handing the engine to another kit track
 * changes it, and a kit loaded meanwhile becomes that track's.
 *
 * Reads the song engine, the kit manager and the instrument host from
 * PlatformServices.
 */
class SongSounds {
public:
    /// How often the platform calls step().
    static constexpr uint32_t kStepMs = 300;

    explicit SongSounds(ISongSoundsHost& host) : host_(host) {}

    /// UI thread, once the kit loader's callbacks are in: a song may have
    /// opened before anyone listened -- the engine opens the last one as soon
    /// as it is first asked for it -- so the one open now is followed too.
    void start();

    /// The song engine's worker, as a song opens.
    void songOpened();

    /// UI thread: one step towards the open song's sounds. Once there: a look
    /// at whether the song was given another kit since, and one at whether a kit
    /// a seq track plays was saved under its own name -- its freeze is then
    /// stale (kit_saved_notify()).
    void step();

    /// UI thread, from a kit load that finished: the open song takes the kit
    /// -- and its live kit track with it --, unless it was given another one
    /// since the last step read it.
    void kitLoaded();

    /// The open song's folder from the card's root ("/SONGS/<name>") when its
    /// Pitch track keeps the instrument; false when no song is open or it has
    /// no Pitch track.
    bool instrumentHome(std::string& songDir);

    /// @p path is the instrument the open song's Pitch track plays now;
    /// written into the song.
    void instrumentIs(const std::string& path);

    /// The instrument host's worker, after a request that succeeded: the open
    /// song's Pitch track keeps @p spec -- its file, or one nobody saved (no
    /// path) as a copy in the song's own folder, written by @p save first.
    /// A song without a Pitch track keeps nothing: browsing samples must not
    /// fill its folder. @p changed: the instrument was built or saved from
    /// edits, so it sounds other than its file name says -- the Pitch
    /// track's freeze (SongTrack::freeze) is of the old sound and goes stale.
    void keepInstrument(const KitInfo& spec, const std::function<bool(const KitInfo&)>& save,
                        bool changed = false);

    /// A kit loaded outside Arrange onto a song with kit tracks is not written
    /// into the song (kitLoaded()): the player chooses on the way into Arrange.
    /// The kit track it would replace -- playing its freeze meanwhile -- or -1
    /// when nothing waits. Gone when another song opens. UI thread.
    int kitChoiceTrack() const;
    /// The kit that waits for the choice. UI thread.
    const std::string& kitChoiceKit() const { return choiceKit_; }
    /// UI thread: @p replace -- the track plays the kit, as a kit loaded in
    /// Arrange does. Otherwise the song stays as it was and the kit waits in
    /// takeNewTrackKit() for Arrange to put it on a kit track of its own.
    void resolveKitChoice(bool replace);
    /// UI thread, Arrange as it shows: the kit a new kit track is to be made
    /// with, once; empty for none.
    std::string takeNewTrackKit();

    /// A step is still to come.
    bool pending() const { return pending_.load(std::memory_order_acquire); }

    /// What is loaded is the open song's: no step to come, no kit load
    /// running. A render of the song (a freeze)
    /// made before would play the sounds of the song open before it.
    bool settled() const;

private:
    std::string logical(const std::string& dir);
    bool foregroundIs(const char* a, const char* b = nullptr);
    void unloadKit();
    void freezesHearKitSaves();
    void seedSavesHeard();

    ISongSoundsHost& host_;
    /// A song opened and the board is not at it yet.
    std::atomic<bool> pending_{false};
    /// Moves with every song that opens: a kit missing from the card is said once a song.
    std::atomic<uint32_t> openGen_{0};
    uint32_t missingSaid_ = UINT32_MAX;   // UI thread
    /// The song's kit as a step last read it: one that differs has been
    /// given to the song since. UI thread.
    std::string lastKit_;
    /// How many saves of each kit.json a step has heard of (kit_saved_generation()):
    /// seeded as a song is adopted; a kit it gains later is taken as it is. UI thread.
    std::unordered_map<std::string, uint32_t> savesHeard_;
    /// The openGen_ savesHeard_ was seeded for. UI thread.
    uint32_t seededGen_ = UINT32_MAX;
    /// The song's instrument when it would not load: asked for again on instRetry_'s
    /// clock (stepMs_, kStepMs a step), said once. UI thread.
    std::string instWanted_;
    RetryBackoff instRetry_;
    uint32_t stepMs_ = 0;
    bool instAsked_ = false;
    bool instSaid_ = false;
    /// The InstrumentPlayState generation the open song's pad map was last
    /// compared with (keepPitchPads()). UI thread.
    uint32_t padsGen_ = UINT32_MAX;
    /// The openGen_ whose song's sounds a step has seen through once -- a kit
    /// changed later sets pending_ again but not this -- and that song's
    /// folder. An open before it is not the player's (keepInstrument()); a
    /// pad map is kept only into the song it was settled for (keepPitchPads()).
    std::atomic<uint32_t> settledGen_{0};
    std::string settledDir_;   // UI thread
    /// kitChoiceTrack(), the openGen_ it was made in, its kit, and the kit
    /// a "new track" answer left for Arrange. UI thread.
    int choiceTrack_ = -1;
    uint32_t choiceGen_ = 0;
    std::string choiceKit_;
    std::string newTrackKit_;
    void keepPitchPads();
};

} // namespace crosspad

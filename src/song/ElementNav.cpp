// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/song/ElementNav.hpp"

#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/song/AudioSong.hpp"
#include "crosspad/song/BakedView.hpp"
#include "crosspad/song/ISongEngine.hpp"
#include "crosspad/song/SongSounds.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

namespace crosspad {

namespace {

/// Polls a request the engine refused is offered again before the job is
/// given up: it refuses while an edit or a bounce runs. Arrange polls every ~50 ms.
constexpr uint32_t kRefusalPolls = 200;
/// Tempos closer than this are the same one.
constexpr float kTempoEpsilon = 0.01f;

/// The tempo a song plays at: a song never given one stores 0 and plays at the
/// grid's default, which is what an element is fitted to and requestTempo can set.
float effectiveBpm(const AudioSong& s) { return s.bpm > 0.0f ? s.bpm : AudioSong::kDefaultBpm; }
/// Polls the card may take to keep an element as it is before it opens.
constexpr uint32_t kSnapshotPolls = 1200;
/// Polls the fitting of stretched blocks may go without one more done before
/// the leave is given up: the card is gone, or the worker is.
constexpr uint32_t kStretchStallPolls = 2400;
/// Polls a level's kit and instrument may take to load before the leave is
/// given up: a kit read from the card, an instrument built from its samples.
constexpr uint32_t kSoundsPolls = 1200;
constexpr char kOtherSong[] = "another song was opened meanwhile";

ISongEngine* songEngine() { return getPlatformServices().songEngine; }

/// A recording keeps to the song's tempo, not the one it was made at: the
/// audio tracks that keep their speed become time-stretched. True when one did.
bool keepRecordingsToTempo(AudioSong& song)
{
    bool changed = false;
    for (SongTrack& t : song.tracks) {
        if ((t.inUse || !t.clips.empty()) && t.kind == TrackKind::Audio && t.stretch == StretchMode::Off) {
            t.stretch = StretchMode::KeepPitch;
            changed = true;
        }
    }
    return changed;
}

/// An id for a new element that no element in the library has.
std::string freshElementRef(ISongEngine& se)
{
    const auto taken = se.elements();
    uint32_t seed = (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    for (;;) {
        const std::string ref = std::string(AudioSong::elementsDir()) + "/" + make_element_id(seed++);
        const bool used = std::any_of(taken.begin(), taken.end(),
                                      [&](const ElementInfo& e) { return e.ref == ref; });
        if (!used) return ref;
    }
}

/// Where an element made of [@p from, @p to) goes: the lowest audio track the
/// region touches that holds nothing outside it (its settings are then the
/// region's own, and its volume, pan and mute are set neutral: the element's
/// bake carries every source track's; its solo stays, or the tracks it silenced
/// would sound), else the lowest track nobody has made. -1 when there is neither.
int regionHost(const AudioSong& song, uint32_t from, uint32_t to, uint32_t spBar)
{
    AudioSong cleared = song;
    cleared.clearRange(from, to, spBar);
    int host = -1;
    for (const RangeClip& k : song.copyRange(from, to, spBar)) {
        if (song.tracks[k.track].kind == TrackKind::Audio && cleared.tracks[k.track].clips.empty() &&
            (host < 0 || k.track < host))
            host = k.track;
    }
    for (int t = 0; host < 0 && t < AudioSong::kTracks; ++t) {
        if (!song.tracks[t].inUse && song.tracks[t].clips.empty()) host = t;
    }
    return host;
}

constexpr char kNoHost[] = "needs a free track, or an audio track with nothing outside the region";

} // namespace

ISongEngine* ElementNav::engine()
{
    ISongEngine* se = songEngine();
    if (!se) error_ = "no song engine in this firmware";
    return se;
}

void ElementNav::syncRoot() const
{
    ISongEngine* se = songEngine();
    if (path_.empty()) path_.emplace_back();
    if (!se || job_ != Job::None) return;
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    const std::string& open = se->song().name;
    if (path_.back().open == open) return;
    /* Another song opened behind the breadcrumb -- the kit browser's SONGS,
     * ASONG OPEN, another app while Arrange was closed: a leave from the
     * element it last knew would bake that song into the element's blocks. */
    path_.assign(1, Level{open, {}, -1, 0});
}

bool ElementNav::onLevel(const std::string& open)
{
    ISongEngine* se = songEngine();
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    return se->song().name == open;
}

const std::vector<ElementNav::Level>& ElementNav::path() const
{
    syncRoot();
    return path_;
}

int ElementNav::depth() const { return (int)path().size() - 1; }

uint8_t ElementNav::progress() const
{
    ISongEngine* se = songEngine();
    if (!se || job_ == Job::None) return 0;
    switch (phase_) {
        case Phase::Freezing: {
            const BounceStatus b = se->bounceStatus();
            return b.framesTotal ? (uint8_t)((uint64_t)b.framesDone * 100 / b.framesTotal) : 0;
        }
        case Phase::Stretching: return se->stretchProgress();
        case Phase::Baking:
        case Phase::Making:     return se->editProgress();
        default:                return 0;
    }
}

const char* ElementNav::stage() const
{
    switch (phase_) {
        case Phase::Making:
        case Phase::Listing:    return "copying";
        case Phase::Stopping:   return "stopping";
        case Phase::Snapshotting: return "keeping";
        case Phase::Opening:
        case Phase::Retempo:    return "opening";
        case Phase::Refit:      return "fitting";
        case Phase::Sounds:     return "loading";
        case Phase::Freezing:   return "freezing";
        case Phase::Stretching:
        case Phase::Baking:     return "baking";
        case Phase::Reopening:  return "linking";
        default:                return "";
    }
}

bool ElementNav::refuse(const std::string& why)
{
    error_ = why;
    return false;
}

bool ElementNav::addBlock(int track, uint32_t bar, uint16_t bars, const std::string& ref)
{
    ISongEngine* se = engine();
    if (!se) return false;
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    AudioSong& song = se->song();
    if (track < 0 || track >= AudioSong::kTracks) return refuse("no such track");
    SongTrack& t = song.tracks[track];
    if (t.inUse || !t.clips.empty()) {
        if (t.kind != TrackKind::Audio) return refuse("a seq track cannot hold an element: use an audio track");
    } else if (!song.addAudioTrack(track)) {
        return refuse("no such track");
    }
    SongClip c;
    c.element = ref;
    c.bar = bar;
    c.bars = bars;
    song.addClip(track, c);
    return true;
}

bool ElementNav::startEnter(const std::string& ref, int track, uint32_t bar, Job job)
{
    ISongEngine* se = engine();
    if (!se) return false;
    if (depth() == 0) {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        songBpm_ = effectiveBpm(se->song());
    }
    ref_ = ref;
    level_ = Level{ref, {}, track, bar};
    job_ = job;
    phase_ = Phase::Stopping;
    asked_ = false;
    refusals_ = 0;
    error_.clear();
    se->stop();
    return true;
}

void ElementNav::startLeave(Job job)
{
    job_ = job;
    ref_ = path_.back().open;
    phase_ = Phase::Stopping;
    asked_ = false;
    refusals_ = 0;
    error_.clear();
    if (ISongEngine* se = songEngine()) se->stop();
}

bool ElementNav::chooseOccurrence(size_t index)
{
    if (busy()) return refuse("busy");
    syncRoot();
    if (depth() == 0) return refuse("there is no element to choose a pass of");
    if (index >= path_.back().occurrences.size()) return refuse("no such pass");
    path_.back().chosen = index;
    askContext();
    return true;
}

void ElementNav::askContext()
{
    ISongEngine* se = songEngine();
    if (!se) return;
    ContextRequest r;
    if (path_.size() > 1) {
        for (const Level& lv : path_) r.chain.push_back(lv.open);
        contextBars_ = se->playEndBars();
        const Level& focus = path_.back();
        if (!focus.occurrences.empty()) {
            /* One pass of the focus, from its frame 0 -- or from the song's
             * first frame, when the pass begins before the song does. */
            const Occurrence& o = focus.occurrences[focus.chosen];
            const int64_t length = bars_to_frames(contextBars_, se->framesPerBar());
            const int64_t lead = o.songFrame < 0 ? -o.songFrame : 0;
            const int64_t at = o.songFrame + lead;
            if (lead < length && at < (int64_t)UINT32_MAX) {
                r.songFrame = (uint32_t)at;
                r.frames = (uint32_t)std::min<int64_t>(length - lead, UINT32_MAX - at);
                r.focusFrame = (uint32_t)lead;
            }
        }
    }
    se->requestContext(r);
}

void ElementNav::followFocusLength()
{
    ISongEngine* se = songEngine();
    if (!se) return;
    syncRoot();
    if (path_.size() < 2 || !onLevel(path_.back().open)) return;
    if (se->playEndBars() != contextBars_) askContext();
}

bool ElementNav::enter(int track, uint32_t bar)
{
    if (busy()) return refuse("busy");
    ISongEngine* se = engine();
    if (!se) return false;
    std::string ref;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const AudioSong& song = se->song();
        if (track < 0 || track >= AudioSong::kTracks) return refuse("no such track");
        for (const SongClip& c : song.tracks[track].clips) {
            if (c.isElement() && c.bar <= bar && bar < c.bar + c.bars) { ref = c.element; break; }
        }
    }
    if (ref.empty()) return refuse("no element there");
    leaveAfterEnter_ = false;
    newTitle_.clear();
    targetDepth_ = 0;
    return startEnter(ref, track, bar, Job::Enter);
}

bool ElementNav::leave()
{
    return leaveTo(depth() - 1);
}

bool ElementNav::leaveTo(int level)
{
    if (busy()) return refuse("busy");
    if (!engine()) return false;
    if (level < 0 || level >= depth()) return refuse("there is no level above");
    targetDepth_ = level;
    startLeave(Job::Leave);
    return true;
}

bool ElementNav::insertNew(int track, uint32_t bar, const std::string& title)
{
    if (busy()) return refuse("busy");
    ISongEngine* se = engine();
    if (!se) return false;
    const std::string ref = freshElementRef(*se);
    if (!addBlock(track, bar, kNewElementBars, ref)) return false;
    se->commit();
    leaveAfterEnter_ = false;
    newTitle_ = title;
    targetDepth_ = 0;
    return startEnter(ref, track, bar, Job::Insert);
}

bool ElementNav::insertExisting(int track, uint32_t bar, const std::string& ref)
{
    if (busy()) return refuse("busy");
    if (!engine()) return false;
    return placeExisting(track, bar, ref);
}

bool ElementNav::placeExisting(int track, uint32_t bar, const std::string& ref)
{
    ISongEngine* se = engine();
    if (!se) return false;
    if (!is_element_ref(ref)) return refuse("not an element");
    syncRoot();
    const auto library = se->elements();
    auto find = [&](const std::string& r) {
        return std::find_if(library.begin(), library.end(), [&](const ElementInfo& e) { return e.ref == r; });
    };
    const auto self = find(ref);
    if (self == library.end()) return refuse("that element is not in the library");
    /* The elements the new block would play, itself included: one of them on
     * the way down would make the element play itself. */
    std::vector<std::string> reached{ref};
    for (size_t i = 0; i < reached.size(); ++i) {
        const bool onPath = std::any_of(path_.begin() + 1, path_.end(),
                                        [&](const Level& lv) { return lv.open == reached[i]; });
        if (onPath) return refuse("an element cannot hold itself");
        const auto info = find(reached[i]);
        if (info == library.end()) continue;
        for (const std::string& use : info->uses)
            if (std::find(reached.begin(), reached.end(), use) == reached.end()) reached.push_back(use);
    }
    if (!addBlock(track, bar, std::max<uint16_t>(1, self->bars), ref)) return false;
    se->commit();
    leaveAfterEnter_ = true;
    newTitle_.clear();
    targetDepth_ = depth();
    return startEnter(ref, track, bar, Job::Insert);
}

bool ElementNav::makeFromRegion(uint32_t fromBar, uint32_t toBar, const std::string& title)
{
    if (busy()) return refuse("busy");
    ISongEngine* se = engine();
    if (!se) return false;
    if (toBar <= fromBar) return refuse("the region is empty");
    syncRoot();
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        track_ = regionHost(se->song(), fromBar, toBar, se->framesPerBar());
        if (track_ < 0) return refuse(kNoHost);
    }
    if (!se->requestElementFromRegion(fromBar, toBar)) return refuse("the element cannot be made now");
    bar_ = fromBar;
    regionBars_ = toBar - fromBar;
    newTitle_ = title;
    startMaking(Job::Make);
    return true;
}

bool ElementNav::insertSong(int track, uint32_t bar, const std::string& songName)
{
    if (busy()) return refuse("busy");
    ISongEngine* se = engine();
    if (!se) return false;
    syncRoot();
    {
        /* Refused before the copy, which would be left in the library. */
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        if (track < 0 || track >= AudioSong::kTracks) return refuse("no such track");
        const SongTrack& t = se->song().tracks[track];
        if ((t.inUse || !t.clips.empty()) && t.kind != TrackKind::Audio)
            return refuse("a seq track cannot hold an element: use an audio track");
    }
    std::vector<std::string> open;
    for (size_t i = 1; i < path_.size(); ++i) open.push_back(path_[i].open);
    if (!se->requestElementFromSong(songName, open)) return refuse("that song cannot be copied now");
    track_ = track;
    bar_ = bar;
    startMaking(Job::Insert);
    return true;
}

void ElementNav::startMaking(Job job)
{
    job_ = job;
    phase_ = Phase::Making;
    asked_ = true;
    refusals_ = 0;
    error_.clear();
}

/// The engine's copy of what the element is made of: when it is on the card
/// the element is laid in the song, as insertNew() and insertExisting() do.
ElementNav::Result ElementNav::making()
{
    ISongEngine* se = songEngine();
    switch (se->editState()) {
        case SongOpState::Busy: return {};
        case SongOpState::Ready: break;
        default: {
            const auto report = se->editReport();
            return fail(report.empty() ? "the element could not be made" : report.front());
        }
    }
    ref_ = se->elementMade();
    if (ref_.empty()) return fail("the element could not be made");
    /* A song opened meanwhile is not what the element was made of. */
    if (!onLevel(path_.back().open)) return failMade(kOtherSong);
    if (job_ == Job::Make) return laidFromRegion();
    /* A copied song is not in the library list yet. */
    if (!se->requestElements()) return failMade("the library cannot be read now");
    phase_ = Phase::Listing;
    refusals_ = 0;
    return {};
}

ElementNav::Result ElementNav::listing()
{
    ISongEngine* se = songEngine();
    if (!onLevel(path_.back().open)) return failMade(kOtherSong);
    /* The copy is the one element no earlier list can hold. */
    const auto library = se->elements();
    const bool listed = std::any_of(library.begin(), library.end(), [&](const ElementInfo& e) { return e.ref == ref_; });
    if (!listed) {
        if (++refusals_ < kRefusalPolls) return {};
        return failMade("the library does not list the copy");
    }
    if (!placeExisting(track_, bar_, ref_)) return failMade(error_);
    return {};
}

/// The region's blocks give way to one block of the new element, in one edit:
/// the host is checked again first, so a refusal leaves the song as it was.
ElementNav::Result ElementNav::laidFromRegion()
{
    ISongEngine* se = songEngine();
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        AudioSong& song = se->song();
        const uint32_t to = bar_ + regionBars_;
        const uint32_t spBar = se->framesPerBar();
        /* A seq track that plays its scene over the whole song has no blocks
         * to give way: it gets them, empty of notes, as the scene plays. */
        AudioSong edited = song;
        edited.seqBlocksExplicit(std::max(song.playEnd(), to));
        if (regionHost(edited, bar_, to, spBar) != track_) return failMade(kNoHost);
        edited.clearRange(bar_, to, spBar);
        SongTrack& host = edited.tracks[track_];
        if (host.inUse) {
            host.volume = 1.0f;
            host.pan = 0.0f;
            host.muted = false;
        } else {
            edited.addAudioTrack(track_);
        }
        SongClip c;
        c.element = ref_;
        c.bar = bar_;
        c.bars = (uint16_t)std::min<uint32_t>(regionBars_, UINT16_MAX);
        edited.addClip(track_, c);
        song = std::move(edited);
    }
    se->commit();
    leaveAfterEnter_ = true;
    targetDepth_ = depth();
    return startEnter(ref_, track_, bar_, Job::Make) ? Result{} : failMade(error_);
}

/// A job that made an element and could not go on: the element is taken off the
/// card again, so no copy is left in the library.
ElementNav::Result ElementNav::failMade(const std::string& why)
{
    if (ISongEngine* se = songEngine()) se->requestElementDiscard(ref_);
    return fail(why);
}

ElementNav::Result ElementNav::fail(const std::string& why)
{
    Result r;
    r.done = true;
    r.job = job_;
    r.error = why;
    error_ = why;
    parentFreeze_ = false;
    job_ = Job::None;
    phase_ = Phase::Idle;
    return r;
}

ElementNav::Result ElementNav::finish()
{
    Result r;
    r.done = true;
    r.ok = true;
    r.job = job_;
    job_ = Job::None;
    phase_ = Phase::Idle;
    return r;
}

ElementNav::Result ElementNav::refused(const std::string& why)
{
    if (++refusals_ < kRefusalPolls) return {};
    return fail(why);
}

ElementNav::Result ElementNav::poll()
{
    ISongEngine* se = songEngine();
    if (job_ == Job::None) {
        /* The first tick: what the library says of itself, for the uses and the
         * snapshots left by an edit a power cut ended. */
        if (se && !usesAsked_) {
            usesAsked_ = true;
            se->requestElements();
            se->requestUses();
        }
        followFocusLength();
        return {};
    }
    if (!se) return fail("no song engine in this firmware");
    switch (phase_) {
        case Phase::Making:     return making();
        case Phase::Listing:    return listing();
        case Phase::Stopping:   return stopped();
        case Phase::Snapshotting: return snapshotting();
        case Phase::Opening:    return opened();
        case Phase::Retempo:    return retempo();
        case Phase::Refit:      return refit();
        case Phase::Sounds:     return sounds();
        case Phase::Freezing:   return freezing();
        case Phase::Stretching: {
            /* Counted afresh, so a copy still to make is never read as none. */
            const uint32_t waiting = se->stretchWaiting();
            if (waiting) {
                if (waiting != lastWaiting_) { lastWaiting_ = waiting; refusals_ = 0; }
                if (++refusals_ < kStretchStallPolls) return {};
                return fail("the audio is not fitted to the song's tempo and the card is not making progress");
            }
            phase_ = Phase::Baking;
            asked_ = false;
            refusals_ = 0;
            return {};
        }
        case Phase::Baking:     return baking();
        case Phase::Reopening:  return reopening();
        default:                return {};
    }
}

/// Entering opens the element once the song is quiet; leaving goes on up.
ElementNav::Result ElementNav::stopped()
{
    ISongEngine* se = songEngine();
    if (se->playing()) return {};
    if (job_ == Job::Leave) return toLeaving();
    /* One stays to be heard around: the song's seq tracks are frozen first, so
     * the context baked from the card finds their freezes valid. */
    if (!leaveAfterEnter_) {
        phase_ = Phase::Freezing;
        parentFreeze_ = true;
        freezeTrack_ = 0;
        asked_ = false;
        refusals_ = 0;
        return {};
    }
    return openChild();
}

/// The element is kept as it is on the card before it opens, the first time
/// only (the request keeps one already there), so that Copy has the original
/// to go back to.
ElementNav::Result ElementNav::toSnapshot()
{
    phase_ = Phase::Snapshotting;
    asked_ = false;
    refusals_ = 0;
    return {};
}

ElementNav::Result ElementNav::snapshotting()
{
    ISongEngine* se = songEngine();
    if (!asked_) {
        if (!onLevel(path_.back().open)) return fail(kOtherSong);
        wasPending_ = se->snapshotPending(ref_);
        if (!se->requestSnapshot(ref_)) return refused("the element cannot be kept as it is now");
        asked_ = true;
        refusals_ = 0;
        return {};
    }
    switch (se->editState()) {
        case SongOpState::Busy:
            if (++refusals_ < kSnapshotPolls) return {};
            return fail("the card is not keeping the element as it is");
        case SongOpState::Ready: break;
        default:                 return fail("the element could not be kept as it is before its edit");
    }
    level_.hashAtEntry = se->snapshotKey();
    level_.snapshotTaken = !wasPending_ && se->snapshotPending(ref_);
    return openChild();
}

/// Every block of the song above, and every pass the focus plays in it.
void ElementNav::snapshotParent(ISongEngine& se)
{
    level_.parentBlocks.clear();
    level_.occurrences.clear();
    level_.chosen = 0;
    std::lock_guard<std::recursive_mutex> l(se.songMutex());
    const AudioSong& song = se.song();
    const uint32_t spb = se.framesPerBar();
    const uint32_t cut = bars_to_frames(song.playEnd(), spb);
    /* What the level above plays of itself: a chosen pass of it, in its own
     * time -- the song above that is all of it. An element above that is
     * heard nowhere has no pass, and nothing of it sounds. */
    const Level& above = path_.back();
    const bool bounded = path_.size() > 1;
    const bool unheard = bounded && above.occurrences.empty();
    const Occurrence heard = bounded && !unheard ? above.occurrences[above.chosen] : Occurrence{};
    const int64_t base = bounded ? heard.songFrame : 0;
    const int64_t heardFrom = heard.offset, heardTo = (int64_t)heard.offset + heard.frames;
    /* A pass with the frames it sounds on in this level's time. */
    struct Pass { Occurrence at; uint32_t from, to; };
    std::vector<Pass> entered, others;
    for (int t = 0; t < AudioSong::kTracks; ++t) {
        const SongTrack& tr = song.tracks[t];
        for (size_t i = 0; i < tr.clips.size(); ++i) {
            const SongClip& c = tr.clips[i];
            const bool focus = c.element == ref_;
            const bool seq = tr.kind == TrackKind::Seq;
            const bool ready = seq ? se.freezeState((uint8_t)t) == FreezeState::Frozen
                                   : !c.isElement() || !c.file.empty();
            level_.parentBlocks.push_back({(uint8_t)t, c.bar, c.bars, c.isElement(), seq, focus, ready});
            if (!focus || !spb || unheard) continue;
            const uint32_t start = bars_to_frames(c.bar, spb);
            const uint32_t end = clip_block_end(tr.clips, i, spb, cut);
            /* A block never baked plays once, over its own length. */
            const uint32_t frames = c.frames ? c.frames : end - start;
            std::vector<Pass>& into = (t == level_.track && c.bar <= level_.bar && level_.bar < c.bar + c.bars)
                ? entered : others;
            element_passes(c.from, frames, spb, start, end, [&](uint32_t at, uint32_t to, uint32_t offset) {
                /* Clipped to what the level above sounds in this pass of itself. */
                const int64_t from = bounded ? std::max<int64_t>(at, heardFrom) : at;
                const int64_t until = bounded ? std::min<int64_t>(to, heardTo) : to;
                if (from >= until) return;
                const uint32_t cutIn = (uint32_t)(from - at);
                into.push_back({{(uint32_t)((uint64_t)from / spb), base + (int64_t)at - (int64_t)offset,
                                 offset + cutIn, (uint32_t)(until - from)},
                                (uint32_t)from, (uint32_t)until});
            });
        }
    }
    std::sort(others.begin(), others.end(),
              [](const Pass& a, const Pass& b) { return a.at.songFrame + a.at.offset < b.at.songFrame + b.at.offset; });
    const uint32_t enteredAt = bars_to_frames(level_.bar, spb);
    for (size_t i = 0; i < entered.size(); ++i)
        if (entered[i].from <= enteredAt && enteredAt < entered[i].to) level_.chosen = i;
    for (const Pass& p : entered) level_.occurrences.push_back(p.at);
    for (const Pass& p : others) level_.occurrences.push_back(p.at);
}

ElementNav::Result ElementNav::openChild()
{
    ISongEngine* se = songEngine();
    snapshotParent(*se);
    if (!se->requestOpenElement(ref_)) return refused("the element cannot be opened now");
    parentFreeze_ = false;
    phase_ = Phase::Opening;
    refusals_ = 0;
    return {};
}

ElementNav::Result ElementNav::opened()
{
    ISongEngine* se = songEngine();
    switch (se->openState()) {
        case SongOpState::Busy:  return {};
        case SongOpState::Ready: break;
        default:                 return fail("the element could not be opened");
    }
    /* From here the engine is on the element: the breadcrumb says so, whatever
     * becomes of the tempo. */
    bool apart = false, other = false;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        AudioSong& song = se->song();
        other = song.name != ref_;
        if (!other) {
            level_.title = song.title;
            apart = std::fabs(effectiveBpm(song) - songBpm_) > kTempoEpsilon;
            if (apart) keepRecordingsToTempo(song);
        }
    }
    if (other) return fail(kOtherSong);
    path_.push_back(level_);
    if (!apart) return entered();
    se->commit();
    phase_ = Phase::Retempo;
    asked_ = false;
    refusals_ = 0;
    return {};
}

ElementNav::Result ElementNav::retempo()
{
    ISongEngine* se = songEngine();
    if (!asked_) {
        if (!onLevel(ref_)) return fail(kOtherSong);
        if (!se->requestTempo(songBpm_)) return refused("the tempo cannot be set now");
        asked_ = true;
        return {};
    }
    switch (se->editState()) {
        case SongOpState::Busy:  return {};
        case SongOpState::Ready: return entered();
        default:                 return fail("the element could not be fitted to the song's tempo");
    }
}

/// The element is open at the song's tempo: a new one is named, an existing
/// one just laid goes straight on to be baked.
ElementNav::Result ElementNav::entered()
{
    ISongEngine* se = songEngine();
    if (!newTitle_.empty()) {
        {
            std::lock_guard<std::recursive_mutex> l(se->songMutex());
            AudioSong& song = se->song();
            song.title = newTitle_;
            /* An element made from a region has its length already. */
            if (job_ != Job::Make) song.length = kNewElementBars;
        }
        se->commit();
        path_.back().title = newTitle_;
    }
    if (!leaveAfterEnter_) {
        askContext();
        return finish();
    }
    return toLeaving();
}

ElementNav::Result ElementNav::toLeaving()
{
    phase_ = Phase::Refit;
    asked_ = false;
    refusals_ = 0;
    return {};
}

/// A tempo turned inside the element -- the top bar, a host's clock, an undo
/// back past the enter -- is put back first: the bake is whole bars at the
/// song's tempo, or its block drifts against the song a little more each pass.
ElementNav::Result ElementNav::refit()
{
    ISongEngine* se = songEngine();
    if (!asked_) {
        bool apart = false, other = false, kept = false;
        {
            std::lock_guard<std::recursive_mutex> l(se->songMutex());
            AudioSong& song = se->song();
            other = song.name != ref_;
            apart = !other && std::fabs(effectiveBpm(song) - songBpm_) > kTempoEpsilon;
            if (apart) kept = keepRecordingsToTempo(song);
        }
        if (other) return fail(kOtherSong);
        if (!apart) return toSounds();
        if (kept) se->commit();
        if (!se->requestTempo(songBpm_)) return refused("the tempo cannot be set now");
        asked_ = true;
        return {};
    }
    switch (se->editState()) {
        case SongOpState::Busy:  return {};
        case SongOpState::Ready: return toSounds();
        default:                 return fail("the element's tempo could not be put back");
    }
}

ElementNav::Result ElementNav::toSounds()
{
    phase_ = Phase::Sounds;
    refusals_ = 0;
    return {};
}

/// The level's kit and instrument are loaded before anything of it is
/// rendered: SongSounds brings them in from a timer after the song opens, and
/// a freeze made before would play the sounds of the level opened before it,
/// stamped as this one's.
ElementNav::Result ElementNav::sounds()
{
    const SongSounds* ss = getPlatformServices().songSounds;
    if (ss && !ss->settled()) {
        if (++refusals_ < kSoundsPolls) return {};
        return fail("the element's kit or instrument did not load");
    }
    phase_ = Phase::Freezing;
    freezeTrack_ = 0;
    asked_ = false;
    refusals_ = 0;
    return {};
}

int ElementNav::nextToFreeze()
{
    ISongEngine* se = songEngine();
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    const AudioSong& song = se->song();
    for (int t = freezeTrack_; t < AudioSong::kTracks; ++t) {
        const SongTrack& tr = song.tracks[t];
        if (tr.kind == TrackKind::Seq && tr.inUse && !song.silenced(t) &&
            se->freezeState((uint8_t)t) != FreezeState::Frozen)
            return t;
    }
    return AudioSong::kTracks;
}

/// A bake mixes a seq track from its freeze: each one not frozen is, in turn.
ElementNav::Result ElementNav::freezing()
{
    ISongEngine* se = songEngine();
    if (!asked_) {
        if (!onLevel(parentFreeze_ ? path_.back().open : ref_)) return fail(kOtherSong);
        freezeTrack_ = nextToFreeze();
        if (freezeTrack_ >= AudioSong::kTracks && parentFreeze_) return toSnapshot();
        if (freezeTrack_ >= AudioSong::kTracks) {
            phase_ = Phase::Stretching;
            lastWaiting_ = 0;
            refusals_ = 0;
            return {};
        }
        if (!se->freezeTrack((uint8_t)freezeTrack_))
            return refused("track " + std::to_string(freezeTrack_ + 1) + " cannot be frozen now");
        asked_ = true;
        refusals_ = 0;
        return {};
    }
    const BounceStatus b = se->bounceStatus();
    if (b.state == BounceState::Running) return {};
    if (b.state == BounceState::Failed) return fail("freezing track " + std::to_string(freezeTrack_ + 1) + ": " + b.error);
    if (se->freezeState((uint8_t)freezeTrack_) != FreezeState::Frozen)
        return fail("track " + std::to_string(freezeTrack_ + 1) + " did not freeze");
    ++freezeTrack_;
    asked_ = false;
    return {};
}

ElementNav::Result ElementNav::baking()
{
    ISongEngine* se = songEngine();
    if (!asked_) {
        if (!onLevel(ref_)) return fail(kOtherSong);
        if (!se->requestBakeElement()) return refused("the song is busy: the element cannot be baked now");
        asked_ = true;
        return {};
    }
    switch (se->editState()) {
        case SongOpState::Busy: return {};
        case SongOpState::Ready: break;
        default: {
            const auto report = se->editReport();
            return fail(report.empty() ? "the element could not be baked" : report.front());
        }
    }
    /* A song opened while the bake was queued is what it mixed. */
    if (!onLevel(ref_)) return fail(kOtherSong);
    const ElementBake bake = se->bakeResult();
    changed_ = bake.key != path_.back().hashAtEntry;
    left_ = ref_;
    bakeFile_ = bake.file;
    bakeFrames_ = bake.frames;
    phase_ = Phase::Reopening;
    asked_ = false;
    refusals_ = 0;
    return {};
}

ElementNav::Result ElementNav::reopening()
{
    ISongEngine* se = songEngine();
    if (!asked_) {
        const size_t parent = path_.size() - 2;
        const std::string& name = path_[parent].open;
        const bool taken = parent == 0 ? se->requestOpen(name) : se->requestOpenElement(name);
        if (!taken) return refused("the song above cannot be opened now");
        asked_ = true;
        return {};
    }
    switch (se->openState()) {
        case SongOpState::Busy:  return {};
        case SongOpState::Ready: break;
        default:                 return fail("the song above could not be opened");
    }
    return linked();
}

/// In the parent every block of the element plays the bake just made.
ElementNav::Result ElementNav::linked()
{
    ISongEngine* se = songEngine();
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        if (se->song().name != path_[path_.size() - 2].open) return fail(kOtherSong);
        for (SongTrack& t : se->song().tracks) {
            for (SongClip& c : t.clips) {
                if (c.element != ref_) continue;
                c.file = bakeFile_;
                c.frames = bakeFrames_;
            }
        }
    }
    se->commit();
    /* Edited inside, the element keeps what it was for the question that
     * follows; left as it was entered, it has nothing to ask. */
    if (!changed_ && path_.back().snapshotTaken) se->requestDropSnapshot(ref_);
    path_.pop_back();
    if (depth() > targetDepth_) {
        ref_ = path_.back().open;
        return toLeaving();
    }
    /* Opening the level dropped the stem of the one left; an element is
     * heard around again (its stem unchanged by what was edited inside it,
     * so found in the cache). */
    askContext();
    se->requestUses();
    return finish();
}

void ElementNav::reset()
{
    path_.clear();
    job_ = Job::None;
    phase_ = Phase::Idle;
    asked_ = false;
    parentFreeze_ = false;
    refusals_ = 0;
    contextBars_ = 0;
    usesAsked_ = false;
    error_.clear();
}

ElementNav& elementNav()
{
    static ElementNav nav;
    return nav;
}

} // namespace crosspad

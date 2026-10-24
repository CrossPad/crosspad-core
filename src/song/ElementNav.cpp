// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/song/ElementNav.hpp"

#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/song/AudioSong.hpp"
#include "crosspad/song/ISongEngine.hpp"

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
/// Polls the fitting of stretched blocks may go without one more done before
/// the leave is given up: the card is gone, or the worker is.
constexpr uint32_t kStretchStallPolls = 2400;

ISongEngine* songEngine() { return getPlatformServices().songEngine; }

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
    if (!se || job_ != Job::None || path_.size() != 1) return;
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    path_[0].open = se->song().name;
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
        case Phase::Baking:     return se->editProgress();
        default:                return 0;
    }
}

const char* ElementNav::stage() const
{
    switch (phase_) {
        case Phase::Stopping:   return "stopping";
        case Phase::Opening:
        case Phase::Retempo:    return "opening";
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
    syncRoot();
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        parentBpm_ = effectiveBpm(se->song());
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

ElementNav::Result ElementNav::fail(const std::string& why)
{
    Result r;
    r.done = true;
    r.job = job_;
    r.error = why;
    error_ = why;
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
    if (job_ == Job::None) return {};
    if (!se) return fail("no song engine in this firmware");
    switch (phase_) {
        case Phase::Stopping:   return stopped();
        case Phase::Opening:    return opened();
        case Phase::Retempo:    return retempo();
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

/// Entering opens the element once the song is quiet; leaving goes on to freeze.
ElementNav::Result ElementNav::stopped()
{
    ISongEngine* se = songEngine();
    if (se->playing()) return {};
    if (job_ == Job::Leave) {
        phase_ = Phase::Freezing;
        freezeTrack_ = 0;
        asked_ = false;
        refusals_ = 0;
        return {};
    }
    if (!se->requestOpenElement(ref_)) return refused("the element cannot be opened now");
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
    bool apart = false;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        level_.title = se->song().title;
        apart = std::fabs(effectiveBpm(se->song()) - parentBpm_) > kTempoEpsilon;
        /* A recording keeps to the song's tempo, not the one it was made at. */
        if (apart) {
            for (SongTrack& t : se->song().tracks)
                if ((t.inUse || !t.clips.empty()) && t.kind == TrackKind::Audio && t.stretch == StretchMode::Off)
                    t.stretch = StretchMode::KeepPitch;
        }
    }
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
        if (!se->requestTempo(parentBpm_)) return refused("the tempo cannot be set now");
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
            song.length = kNewElementBars;
        }
        se->commit();
        path_.back().title = newTitle_;
    }
    if (!leaveAfterEnter_) return finish();
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
        freezeTrack_ = nextToFreeze();
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
    const ElementBake bake = se->bakeResult();
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
        for (SongTrack& t : se->song().tracks) {
            for (SongClip& c : t.clips) {
                if (c.element != ref_) continue;
                c.file = bakeFile_;
                c.frames = bakeFrames_;
            }
        }
    }
    se->commit();
    path_.pop_back();
    if (depth() > targetDepth_) {
        ref_ = path_.back().open;
        phase_ = Phase::Freezing;
        freezeTrack_ = 0;
        asked_ = false;
        refusals_ = 0;
        return {};
    }
    return finish();
}

void ElementNav::reset()
{
    path_.clear();
    job_ = Job::None;
    phase_ = Phase::Idle;
    asked_ = false;
    refusals_ = 0;
    error_.clear();
}

ElementNav& elementNav()
{
    static ElementNav nav;
    return nav;
}

} // namespace crosspad

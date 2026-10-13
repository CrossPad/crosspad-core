// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "crosspad/transport/SequencerTransport.hpp"

using crosspad::SequencerController;
using crosspad::SequencerMode;
using crosspad::SequencerState;
using crosspad::SequencerTransport;
using crosspad::TransportState;

/* No engine is attached: the mapping under test reads only the controller's
 * own mode_/state_, which setMode()/setState() write directly. An engine
 * would add 56 stubbed virtuals and test nothing more. */

namespace {

TransportState mapped(SequencerState st, SequencerMode mode)
{
    SequencerController seq;
    /* setState(Idle) clears the mode, so mode goes on after the state -- the
     * same order the engine's callbacks use. */
    seq.setState(st);
    seq.setMode(mode);
    SequencerTransport t(seq);
    return t.state();
}

} // namespace

TEST_CASE("sequencer transport: idle is stopped whatever the mode says", "[transport]")
{
    /* setState(Idle) is what clears a stale mode; the bar must not show a
     * recording transport because Record outlived the stop. */
    REQUIRE(mapped(SequencerState::Idle, SequencerMode::None) == TransportState::Stopped);
    REQUIRE(mapped(SequencerState::Idle, SequencerMode::Record) == TransportState::Stopped);
    REQUIRE(mapped(SequencerState::Idle, SequencerMode::Overdub) == TransportState::Stopped);
}

TEST_CASE("sequencer transport: the three waits are all Waiting", "[transport]")
{
    for (auto mode : {SequencerMode::None, SequencerMode::Playback,
                      SequencerMode::Record, SequencerMode::Overdub}) {
        REQUIRE(mapped(SequencerState::WaitForSync, mode) == TransportState::Waiting);
        REQUIRE(mapped(SequencerState::WaitNote, mode) == TransportState::Waiting);
        REQUIRE(mapped(SequencerState::PreBeat, mode) == TransportState::Waiting);
    }
}

TEST_CASE("sequencer transport: running splits by mode", "[transport]")
{
    REQUIRE(mapped(SequencerState::Running, SequencerMode::Playback) == TransportState::Playing);
    REQUIRE(mapped(SequencerState::Running, SequencerMode::None) == TransportState::Playing);
    REQUIRE(mapped(SequencerState::Running, SequencerMode::Record) == TransportState::Recording);
    REQUIRE(mapped(SequencerState::Running, SequencerMode::Overdub) == TransportState::Overdubbing);
    /* Counting to the overdub's entry while the pattern under it already
     * sounds: Overdubbing, not Waiting -- something IS playing. */
    REQUIRE(mapped(SequencerState::Running, SequencerMode::OverdubWait) == TransportState::Overdubbing);
}

TEST_CASE("sequencer transport: the keys it offers follow the state", "[transport]")
{
    using namespace crosspad;
    SequencerController seq;
    SequencerTransport t(seq);
    REQUIRE(std::string(t.name()) == "PATTERN");

    /* Stopped: nothing runs to layer onto, so no DUB. STOP stays -- a second
     * one rewinds. */
    REQUIRE(t.caps() == (kTransportCanPlay | kTransportCanStop | kTransportCanRecord));

    /* Armed: only the two keys that cancel. */
    seq.setState(SequencerState::PreBeat);
    seq.setMode(SequencerMode::Record);
    REQUIRE(t.caps() == (kTransportCanStop | kTransportCanRecord));

    seq.setState(SequencerState::Running);
    seq.setMode(SequencerMode::Playback);
    REQUIRE(t.caps() == (kTransportCanPlay | kTransportCanStop |
                         kTransportCanRecord | kTransportCanOverdub));
}

TEST_CASE("sequencer transport: isRecording and the bar agree", "[transport]")
{
    /* Two ways of asking the same question lived side by side before the
     * transport; they must not drift apart. */
    for (auto st : {SequencerState::Idle, SequencerState::WaitForSync,
                    SequencerState::WaitNote, SequencerState::PreBeat,
                    SequencerState::Running}) {
        for (auto mode : {SequencerMode::None, SequencerMode::Playback,
                          SequencerMode::Record, SequencerMode::Overdub,
                          SequencerMode::OverdubWait}) {
            SequencerController seq;
            seq.setState(st);
            seq.setMode(mode);
            SequencerTransport t(seq);
            const bool barSaysTake = t.state() == TransportState::Recording ||
                                     t.state() == TransportState::Overdubbing;
            if (seq.isRecording()) REQUIRE(barSaysTake);
        }
    }
}

/* ── Tempo and position ──────────────────────────────────────────────────── */

namespace {

/// The nineteen pure virtuals and nothing else: `progress()` and the tempo
/// are all the readouts need, and a stub that answers more would be a second
/// sequencer to keep in step with the first.
class EngineStub : public crosspad::ISequencerEngine {
public:
    float progressValue = 0.0f;
    float bpmValue = 120.0f;
    int   plays = 0, stops = 0, records = 0, overdubs = 0, rewinds = 0;

    void setLength(uint8_t) override {}
    void setQuantization(crosspad::Quantization) override {}
    void setLoop(bool) override {}
    void setPreLen(uint8_t) override {}
    void selectSeqRecord(uint8_t) override {}
    void selectDirect(uint8_t, bool) override {}
    void select(uint8_t) override {}
    void playback() override { ++plays; }
    void record() override   { ++records; }
    void overdub() override  { ++overdubs; }
    void stop() override     { ++stops; }
    void reset() override    {}
    void clearAll(uint8_t, uint8_t) override {}
    void clearSeq(uint8_t, uint8_t) override {}
    void setClickAudible(bool) override {}
    void setClickBpm(float bpm) override { bpmValue = bpm; }
    void resetClick() override { ++rewinds; }
    float getClickBpm() const override { return bpmValue; }
    int getClickPos() const override { return 0; }

    float progress() const override { return progressValue; }

    /* Not pure, and the defaults answer 0 -- which made the counted-in take
     * look like a plain one and failed the test rather than the code. */
    uint8_t bars = 4;
    uint8_t sceneBars(uint8_t) const override { return bars; }
    uint8_t activeScene() const override { return 0; }

    void    setCountIn(bool on) override { countIn = on; }
    uint8_t countInBeats() const override { return countIn ? 4 : 0; }
    bool    countIn = false;
};

struct Rig {
    EngineStub engine;
    SequencerController seq;
    SequencerTransport transport{seq};

    explicit Rig(uint8_t bars) {
        /* The scene's bars are what the readout divides by, so the stub
         * answers sceneBars() -- setSeqLength() is the global maximum and
         * using it here is exactly the bug the screen showed. */
        engine.bars = bars;
        seq.setSeqLength(bars);
        seq.init(engine);
    }
};

} // namespace

TEST_CASE("sequencer transport: bar.beat.cent comes out of one progress number", "[transport]")
{
    Rig r(4);   // four bars, four beats each = 16 beats

    r.engine.progressValue = 0.0f;
    auto p = r.transport.position();
    REQUIRE(p.bar == 1);
    REQUIRE(p.beat == 1);
    REQUIRE(p.cent == 0);

    /* Exactly halfway: beat 8 of 16, which a musician counts as bar 3 beat 1. */
    r.engine.progressValue = 0.5f;
    p = r.transport.position();
    REQUIRE(p.bar == 3);
    REQUIRE(p.beat == 1);
    REQUIRE(p.cent == 0);

    /* One beat in. */
    r.engine.progressValue = 1.0f / 16.0f;
    p = r.transport.position();
    REQUIRE(p.bar == 1);
    REQUIRE(p.beat == 2);

    /* Last beat of the last bar. */
    r.engine.progressValue = 15.0f / 16.0f;
    p = r.transport.position();
    REQUIRE(p.bar == 4);
    REQUIRE(p.beat == 4);
}

TEST_CASE("sequencer transport: the loop point never shows a bar that cannot exist", "[transport]")
{
    Rig r(2);
    /* progress() reads exactly 1.0 at the wrap; bar 3 of a two-bar pattern
     * would be a number the user cannot act on. */
    r.engine.progressValue = 1.0f;
    const auto p = r.transport.position();
    REQUIRE(p.bar == 1);
    REQUIRE(p.beat == 1);
    REQUIRE(p.cent == 0);
}

TEST_CASE("sequencer transport: cent stays inside two digits", "[transport]")
{
    Rig r(1);
    /* Just short of the next beat: the field is two digits wide and 100 would
     * both overflow it and read as the beat after. */
    for (float f : {0.2499f, 0.24999f, 0.2499999f}) {
        const auto p = r.transport.position();
        (void)p;
        r.engine.progressValue = f;
        REQUIRE(r.transport.position().cent <= 99);
    }
}

TEST_CASE("sequencer transport: a zero-length pattern does not divide by it", "[transport]")
{
    Rig r(0);
    r.engine.progressValue = 0.7f;
    const auto p = r.transport.position();
    REQUIRE(p.bar == 1);
    REQUIRE(p.beat == 1);
}

TEST_CASE("sequencer transport: tempo is the engine's, and commands reach it", "[transport]")
{
    Rig r(4);
    r.engine.bpmValue = 93.5f;
    REQUIRE(r.transport.bpm() == 93.5f);

    crosspad::transportClaim(&r.transport);
    crosspad::transportPlay();
    /* The stub runs nothing; the controller is told what the engine would
     * have said, so the next command meets the state it would on a board. */
    r.seq.setState(SequencerState::Running);
    r.seq.setMode(SequencerMode::Playback);
    crosspad::transportOverdub();
    crosspad::transportStop();
    crosspad::transportRelease(&r.transport);

    REQUIRE(r.engine.plays == 1);
    REQUIRE(r.engine.overdubs == 1);
    REQUIRE(r.engine.stops == 1);
}

/* ── UX 2000 section 3.1, row by row ─────────────────────────────────────── */

namespace {
void put(Rig& r, SequencerState st, SequencerMode m)
{
    r.seq.setState(st);
    r.seq.setMode(m);
}
} // namespace

TEST_CASE("sequencer transport: stopped -- STOP rewinds, DUB does nothing", "[transport]")
{
    Rig r(4);
    const int before = r.engine.rewinds;   // init() resets the click once
    r.transport.stop();
    REQUIRE(r.engine.rewinds == before + 1);
    REQUIRE(r.engine.stops == 0);
    r.transport.overdub();
    REQUIRE(r.engine.overdubs == 0);
    r.transport.record();
    REQUIRE(r.engine.records == 1);
}

TEST_CASE("sequencer transport: waiting -- REC and STOP cancel, PLAY and DUB do nothing", "[transport]")
{
    Rig r(4);
    put(r, SequencerState::PreBeat, SequencerMode::Record);
    r.transport.play();
    r.transport.overdub();
    REQUIRE(r.engine.plays == 0);
    REQUIRE(r.engine.overdubs == 0);
    r.transport.record();
    REQUIRE(r.engine.stops == 1);
    REQUIRE(r.engine.records == 0);

    put(r, SequencerState::WaitNote, SequencerMode::Record);
    const int before = r.engine.rewinds;
    r.transport.stop();
    REQUIRE(r.engine.stops == 2);
    REQUIRE(r.engine.rewinds == before);
}

TEST_CASE("sequencer transport: playing -- REC punches in, DUB layers, PLAY does nothing", "[transport]")
{
    Rig r(4);
    put(r, SequencerState::Running, SequencerMode::Playback);
    r.transport.play();
    REQUIRE(r.engine.plays == 0);
    r.transport.record();
    REQUIRE(r.engine.records == 1);
    r.transport.overdub();
    REQUIRE(r.engine.overdubs == 1);
}

TEST_CASE("sequencer transport: recording -- DUB layers, PLAY plays on", "[transport]")
{
    Rig r(4);
    put(r, SequencerState::Running, SequencerMode::Record);
    r.transport.play();
    REQUIRE(r.engine.plays == 1);       // punched out, the loop on
    put(r, SequencerState::Running, SequencerMode::Record);
    r.transport.overdub();
    REQUIRE(r.engine.overdubs == 1);
}

TEST_CASE("sequencer transport: overdubbing -- REC records, DUB plays, PLAY plays on", "[transport]")
{
    Rig r(4);
    put(r, SequencerState::Running, SequencerMode::Overdub);
    r.transport.play();
    REQUIRE(r.engine.plays == 1);       // out of the layering, the loop on
    put(r, SequencerState::Running, SequencerMode::Overdub);
    r.transport.record();
    REQUIRE(r.engine.records == 1);
    r.transport.overdub();
    REQUIRE(r.engine.plays == 2);
    REQUIRE(r.engine.overdubs == 0);
}

/* ── The four rules that moved here from the Sequencer screen ─────────────
 *
 * They were lost for a build when the transport moved to the top bar: the bar
 * called plain playback()/record() and the song-synced start, the counted-in
 * take, the second REC that closes a take and the stop that takes the song
 * with it all quietly stopped happening. These are the guard.
 */

#include "crosspad/song/ISongEngine.hpp"
#include "crosspad/transport/SongTransport.hpp"

namespace {

class SongStub : public crosspad::ISongEngine {
public:
    int  withTake = 0, countedIn = 0, stops = 0;
    crosspad::SyncTake lastTake = crosspad::SyncTake::None;
    uint8_t lastBeats = 0;
    bool    isPlaying = false;

    bool playWithTake(uint32_t, crosspad::SyncTake take) override
    {
        ++withTake; lastTake = take; isPlaying = true; return true;
    }
    bool playCountedIn(uint32_t, crosspad::SyncTake take, uint8_t beats) override
    {
        ++countedIn; lastTake = take; lastBeats = beats; isPlaying = true; return true;
    }
    void stop() override { ++stops; isPlaying = false; }
    bool playing() const override { return isPlaying; }

    bool requestProjects() override { return true; }
    std::vector<std::string> projects() override { return {}; }
    bool requestOpen(const std::string&) override { return true; }
    crosspad::SongOpState openState() const override { return crosspad::SongOpState::Ready; }
    crosspad::AudioSong& song() override { return song_; }
    std::recursive_mutex& songMutex() override { return m_; }
    void commit() override {}
    bool requestClips() override { return true; }
    std::vector<crosspad::ClipInfo> clips() override { return {}; }
    uint32_t listGeneration() const override { return 0; }
    bool bounceScene(uint8_t, uint8_t) override { return false; }
    crosspad::BounceStatus bounceStatus() override { return {}; }
    void cancelBounce() override {}
    bool play(uint32_t) override { return false; }
    double positionBars() const override { return 0.0; }
    uint32_t underruns() const override { return 0; }
    uint32_t openFailures() const override { return 0; }

private:
    crosspad::AudioSong song_;
    std::recursive_mutex m_;
};

/// A song the transport is allowed to start: SYNC on and something to play.
void arm_sync(SongStub& st, bool on, uint32_t bars = 4)
{
    std::lock_guard<std::recursive_mutex> l(st.songMutex());
    st.song().syncSeq = on;
    st.song().length = bars;
}

/// The transport reads the song through PlatformServices, the way it does on
/// a board; cleared again so one test cannot leak into the next.
struct SongScope {
    explicit SongScope(SongStub* st) { crosspad::getPlatformServices().songEngine = st; }
    ~SongScope() { crosspad::getPlatformServices().songEngine = nullptr; }
};

} // namespace

TEST_CASE("sequencer transport: a second REC closes the take and keeps the loop", "[transport]")
{
    Rig r(4);
    r.seq.setState(SequencerState::Running);
    r.seq.setMode(SequencerMode::Record);
    REQUIRE(r.seq.isRecording());

    r.transport.record();
    /* playback(), not record(): the take closes and the pattern keeps
     * running. A second record() here would have restarted the take. */
    REQUIRE(r.engine.plays == 1);
    REQUIRE(r.engine.records == 0);
}

TEST_CASE("sequencer transport: in sync the song starts, not the sequencer", "[transport]")
{
    Rig r(4);
    SongStub song;
    SongScope scope(&song);
    arm_sync(song, true);

    r.transport.play();
    REQUIRE(song.withTake == 1);
    REQUIRE(song.lastTake == crosspad::SyncTake::None);
    REQUIRE(r.engine.plays == 0);   // the song starts it, with itself
}

TEST_CASE("sequencer transport: a synced take counts in through the song", "[transport]")
{
    Rig r(4);
    SongStub song;
    SongScope scope(&song);
    arm_sync(song, true);
    r.seq.setCountIn(true);

    r.transport.record();
    REQUIRE(song.countedIn == 1);
    REQUIRE(song.lastTake == crosspad::SyncTake::Record);
    REQUIRE(song.lastBeats == r.seq.getCountInBeats());
    REQUIRE(r.engine.records == 0);
}

TEST_CASE("sequencer transport: stopping in sync stops the song too", "[transport]")
{
    Rig r(4);
    SongStub song;
    SongScope scope(&song);
    arm_sync(song, true);
    r.transport.play();            // the song is running now, and starts the pattern
    r.seq.setState(SequencerState::Running);
    r.seq.setMode(SequencerMode::Playback);

    r.transport.stop();
    REQUIRE(song.stops == 1);
    REQUIRE(r.engine.stops == 1);  // and the pattern, always
}

TEST_CASE("sequencer transport: SYNC off keeps the song out of it", "[transport]")
{
    Rig r(4);
    SongStub song;
    SongScope scope(&song);
    arm_sync(song, false);

    r.transport.play();
    r.seq.setState(SequencerState::Running);
    r.seq.setMode(SequencerMode::Playback);
    r.transport.stop();
    REQUIRE(song.withTake == 0);
    REQUIRE(song.countedIn == 0);
    REQUIRE(song.stops == 0);
    REQUIRE(r.engine.plays == 1);
    REQUIRE(r.engine.stops == 1);
}

TEST_CASE("sequencer transport: a song with nothing in it is not in sync", "[transport]")
{
    Rig r(4);
    SongStub song;
    SongScope scope(&song);
    /* SYNC on but playEnd() == 0: there is nothing for the song to start, so
     * the sequencer goes on its own rather than nowhere. */
    arm_sync(song, true, 0);

    r.transport.play();
    REQUIRE(song.withTake == 0);
    REQUIRE(r.engine.plays == 1);
}

TEST_CASE("sequencer transport: the readout divides by the scene, not the maximum", "[transport]")
{
    /* The controller's seqLength_ is a global maximum -- the platform sets it
     * to MAX_SEQ_LENGTH at init -- while each scene has bars of its own.
     * Reading the wrong one put a one-bar scene at bar 8 on the screen, and
     * every test passed because the rig set both to the same number. They
     * differ here on purpose. */
    Rig r(1);                 // the scene is one bar
    r.seq.setSeqLength(8);    // the controller's maximum is eight

    r.engine.progressValue = 0.75f;   // three quarters through one bar
    const auto p = r.transport.position();
    REQUIRE(p.bar == 1);
    REQUIRE(p.beat == 4);
}

TEST_CASE("sequencer transport: an Arrange track's seq view plays the song, SYNC or not", "[transport]")
{
    Rig r(4);
    SongStub song;
    SongScope scope(&song);
    arm_sync(song, false);           // SYNC off
    r.transport.setFollowSong(true);

    r.transport.record();
    REQUIRE(song.withTake + song.countedIn == 1);
    REQUIRE(song.lastTake == crosspad::SyncTake::Record);
    REQUIRE(r.engine.records == 0);  // the song starts the take, on its bar one

    /* Primed but not yet started: armed, not stopped. */
    REQUIRE(r.transport.state() == TransportState::Waiting);
}

TEST_CASE("transport: a claim does not stop a holder that is already stopped", "[transport]")
{
    Rig r(4);
    const int before = r.engine.rewinds;
    crosspad::transportClaim(&r.transport);
    SongStub song;
    SongScope scope(&song);
    crosspad::SongTransport other;
    crosspad::transportClaim(&other);   // the pattern was stopped: no stop, so no rewind
    REQUIRE(r.engine.rewinds == before);
    REQUIRE(r.engine.stops == 0);
    crosspad::transportRelease(&other);
}

// ── the song's transport: REC and DUB onto the seq track ───────────────────

namespace {

class TakeStub : public SongStub {
public:
    int records = 0, punches = 0;
    uint32_t lastFrom = 0;
    uint8_t  lastCountIn = 0xFF;
    crosspad::SyncTake take = crosspad::SyncTake::None, lastPunch = crosspad::SyncTake::None;
    bool punchable = true;

    bool recordSeq(uint32_t fromBar, crosspad::SyncTake t, uint8_t beats) override
    {
        ++records; lastFrom = fromBar; lastCountIn = beats; take = t; isPlaying = true; return true;
    }
    bool seqPunch(crosspad::SyncTake t) override { ++punches; lastPunch = t; take = t; return true; }
    crosspad::SyncTake seqTake() const override { return take; }
    bool seqPunchable() const override { return punchable; }
};

} // namespace

TEST_CASE("song transport: REC records from the playhead, DUB as well", "[transport][songtake]")
{
    TakeStub song;
    SongScope scope(&song);
    crosspad::SongTransport t;
    t.setStartBar(4);                            // the marker on bar five
    REQUIRE((t.caps() & crosspad::kTransportCanRecord));
    REQUIRE((t.caps() & crosspad::kTransportCanOverdub));
    t.record();
    REQUIRE(song.records == 1);
    REQUIRE(song.lastFrom == 4);
    REQUIRE(song.take == crosspad::SyncTake::Record);
    REQUIRE(t.state() == TransportState::Recording);
    song.stop();
    song.take = crosspad::SyncTake::None;
    t.overdub();
    REQUIRE(song.records == 2);
    REQUIRE(song.take == crosspad::SyncTake::Overdub);
    REQUIRE(t.state() == TransportState::Overdubbing);
}

TEST_CASE("song transport: looping a region, a take starts at the region", "[transport][songtake]")
{
    TakeStub song;
    SongScope scope(&song);
    {
        std::lock_guard<std::recursive_mutex> l(song.songMutex());
        song.song().loop = true;
        song.song().loopStart = 2;
        song.song().loopEnd = 4;
    }
    crosspad::SongTransport t;
    t.setStartBar(7);
    t.record();
    REQUIRE(song.lastFrom == 2);
}

TEST_CASE("song transport: playing, REC punches in and out, PLAY plays on", "[transport][songtake]")
{
    TakeStub song;
    SongScope scope(&song);
    crosspad::SongTransport t;
    song.isPlaying = true;
    REQUIRE(t.state() == TransportState::Playing);
    t.record();
    REQUIRE(song.lastPunch == crosspad::SyncTake::Record);
    t.overdub();                                 // a REC turned into a DUB from here
    REQUIRE(song.lastPunch == crosspad::SyncTake::Overdub);
    t.overdub();                                 // out again
    REQUIRE(song.lastPunch == crosspad::SyncTake::None);
    t.record();
    t.play();                                    // PLAY ends the take, the song plays on
    REQUIRE(song.lastPunch == crosspad::SyncTake::None);
    REQUIRE(song.isPlaying);
    REQUIRE(song.records == 0);
}

TEST_CASE("song transport: no track for a take, no REC", "[transport][songtake]")
{
    TakeStub song;
    SongScope scope(&song);
    {
        std::lock_guard<std::recursive_mutex> l(song.songMutex());
        for (int i = 0; i < crosspad::AudioSong::kTracks; ++i) REQUIRE(song.song().addAudioTrack(i));
    }
    crosspad::SongTransport t;
    REQUIRE_FALSE((t.caps() & crosspad::kTransportCanRecord));
    /* Playing without the sequencer locked to it: nothing to punch into. */
    song.isPlaying = true;
    song.punchable = false;
    REQUIRE_FALSE((t.caps() & crosspad::kTransportCanRecord));
}

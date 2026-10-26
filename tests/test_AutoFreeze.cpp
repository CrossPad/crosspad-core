#include <catch2/catch_test_macros.hpp>

#include "crosspad/song/AutoFreeze.hpp"

using namespace crosspad;

namespace {
AudioSong twoKits() {
    AudioSong s;
    s.bpm = 120.0f;
    REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
    REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 1, "/crosspad/kits/B/kit.json"));
    SeqNote n;
    n.pad = 1;
    n.vel = 100;
    s.putSeqTake(0, 0, makeSeqPattern(1, {n}), s.samplesPerBar(44100), 1);
    s.putSeqTake(1, 0, makeSeqPattern(1, {n}), s.samplesPerBar(44100), 1);
    s.armed = 1;
    return s;
}
} // namespace

TEST_CASE("auto-freeze: the track ARM is not on, never frozen, is next", "[song][freeze]") {
    const AudioSong s = twoKits();
    const FreezeState st[AudioSong::kTracks] = {};
    CHECK(autoFreezeNext(s, st, 0) == 0);
}

TEST_CASE("auto-freeze: a stale freeze is made again; a frozen one and the armed one are not", "[song][freeze]") {
    const AudioSong s = twoKits();
    FreezeState st[AudioSong::kTracks] = {FreezeState::Stale, FreezeState::None};   // the tempo changed
    CHECK(autoFreezeNext(s, st, 0) == 0);
    st[0] = FreezeState::Frozen;
    CHECK(autoFreezeNext(s, st, 0) == -1);                // track 1 is armed: the fingers play it
}

TEST_CASE("auto-freeze: one at a time, and a track skipped waits", "[song][freeze]") {
    AudioSong s = twoKits();
    s.armed = -1;
    FreezeState st[AudioSong::kTracks] = {FreezeState::None, FreezeState::Freezing};
    CHECK(autoFreezeNext(s, st, 0) == -1);
    st[1] = FreezeState::None;
    CHECK(autoFreezeNext(s, st, 1u << 0) == 1);           // track 0's last try failed
}

TEST_CASE("auto-freeze: a kit of its own waits for the second kit engine", "[song][freeze]") {
    AudioSong s = twoKits();
    REQUIRE(s.addSeqTrack(2, SeqSource::Kit, 2, "/crosspad/kits/C/kit.json"));
    SeqNote n;
    n.pad = 1;
    n.vel = 100;
    s.putSeqTake(2, 0, makeSeqPattern(1, {n}), s.samplesPerBar(44100), 1);
    s.armed = -1;
    REQUIRE(s.kitSlot == 1);                              // the first kit of its own holds it
    FreezeState st[AudioSong::kTracks] = {FreezeState::Frozen, FreezeState::Frozen};
    CHECK(autoFreezeNext(s, st, 0) == -1);                // track 2 has no engine to render it
    s.kitSlot = 2;
    CHECK(autoFreezeNext(s, st, 0) == 2);
}

TEST_CASE("auto-freeze: an empty kit track and an audio track are never frozen", "[song][freeze]") {
    AudioSong s;
    REQUIRE(s.addAudioTrack(0));
    REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 0));         // made, nothing recorded yet
    s.armed = -1;
    const FreezeState st[AudioSong::kTracks] = {};
    CHECK(autoFreezeNext(s, st, 0) == -1);
}

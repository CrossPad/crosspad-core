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
    REQUIRE(s.kitSlots[0] == 1);                              // the first kit of its own holds it
    s.kitSlots[1] = -1;                                       // the other engine busy elsewhere
    FreezeState st[AudioSong::kTracks] = {FreezeState::Frozen, FreezeState::Frozen};
    CHECK(autoFreezeNext(s, st, 0) == -1);                // track 2 has no engine to render it
    s.kitSlots[0] = 2;
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

namespace {
/// Track 0 on the pads' kit; 1, 2, 3 kits of their own, each with a take.
AudioSong fourKits() {
    AudioSong s = twoKits();
    SeqNote n;
    n.pad = 2;
    n.vel = 100;
    REQUIRE(s.addSeqTrack(2, SeqSource::Kit, 2, "/crosspad/kits/C/kit.json"));
    REQUIRE(s.addSeqTrack(3, SeqSource::Kit, 3, "/crosspad/kits/D/kit.json"));
    s.putSeqTake(2, 0, makeSeqPattern(1, {n}), s.samplesPerBar(44100), 1);
    s.putSeqTake(3, 0, makeSeqPattern(1, {n}), s.samplesPerBar(44100), 1);
    return s;
}
} // namespace

TEST_CASE("kit slots: a new kit track takes the free slot, then none is left", "[song][freeze][slots]") {
    AudioSong s = fourKits();
    CHECK(s.kitSlots[0] == 1);
    CHECK(s.kitSlots[1] == 2);
    CHECK(s.liveRoute(1) == 1);
    CHECK(s.liveRoute(2) == 3);
    CHECK(s.liveRoute(3) == -1);
    CHECK(s.seqTrackOnRoute(3) == 2);
}

TEST_CASE("kit slots: ARM on a track without one takes a slot at once, the one it left keeps the other",
          "[song][freeze][slots]") {
    AudioSong s = fourKits();
    s.kitSlots[0] = 1;
    s.kitSlots[1] = -1;
    const FreezeState st[AudioSong::kTracks] = {FreezeState::Frozen, FreezeState::Stale, FreezeState::Frozen,
                                                FreezeState::None};
    int8_t slots[AudioSong::kKitSlots];
    CHECK(kitSlotsNext(s, st, 3, 1, 0, false, slots));
    CHECK(slots[0] == 1);                                 // stale: it plays on, live, until it is frozen
    CHECK(slots[1] == 3);
}

TEST_CASE("kit slots: while the song plays a frozen track keeps its kit loaded, for ARM to come back at once",
          "[song][freeze][slots]") {
    AudioSong s = fourKits();
    s.kitSlots[0] = 1;
    s.kitSlots[1] = 3;
    const FreezeState st[AudioSong::kTracks] = {FreezeState::Frozen, FreezeState::Frozen, FreezeState::Frozen,
                                                FreezeState::Frozen};
    int8_t slots[AudioSong::kKitSlots];
    CHECK_FALSE(kitSlotsNext(s, st, 3, 1, 0, true, slots));
    CHECK(slots[0] == 1);
    s.armed = 1;
    CHECK_FALSE(kitSlotsNext(s, st, 1, 3, 0, true, slots));   // back to it: it has its engine already
}

TEST_CASE("kit slots: a frozen track gives its slot back; the armed one keeps its own", "[song][freeze][slots]") {
    AudioSong s = fourKits();
    s.kitSlots[0] = 1;
    s.kitSlots[1] = 3;
    const FreezeState st[AudioSong::kTracks] = {FreezeState::Frozen, FreezeState::Frozen, FreezeState::Frozen,
                                                FreezeState::Frozen};
    int8_t slots[AudioSong::kKitSlots];
    CHECK(kitSlotsNext(s, st, 3, 1, 0, false, slots));
    CHECK(slots[0] == -1);
    CHECK(slots[1] == 3);
}

TEST_CASE("kit slots: with both held live, ARM takes the slot of the track played before last",
          "[song][freeze][slots]") {
    AudioSong s = fourKits();
    s.kitSlots[0] = 1;
    s.kitSlots[1] = 2;
    const FreezeState st[AudioSong::kTracks] = {FreezeState::Frozen, FreezeState::Stale, FreezeState::Stale,
                                                FreezeState::Stale};
    int8_t slots[AudioSong::kKitSlots];
    CHECK(kitSlotsNext(s, st, 3, 2, 0, false, slots));           // the fingers were on 2, before that on 1
    CHECK(slots[0] == 3);
    CHECK(slots[1] == 2);
}

TEST_CASE("kit slots: a frozen holder goes before a live one", "[song][freeze][slots]") {
    AudioSong s = fourKits();
    s.kitSlots[0] = 1;
    s.kitSlots[1] = 2;
    const FreezeState st[AudioSong::kTracks] = {FreezeState::Frozen, FreezeState::Stale, FreezeState::Frozen,
                                                FreezeState::Stale};
    int8_t slots[AudioSong::kKitSlots];
    CHECK(kitSlotsNext(s, st, 3, 1, 0, false, slots));
    CHECK(slots[0] == 1);
    CHECK(slots[1] == 3);
}

TEST_CASE("kit slots: a free slot goes to a stale track nobody plays, so it is heard and can be frozen",
          "[song][freeze][slots]") {
    AudioSong s = fourKits();
    s.armed = 0;
    s.kitSlots[0] = -1;
    s.kitSlots[1] = -1;
    FreezeState st[AudioSong::kTracks] = {FreezeState::Frozen, FreezeState::Frozen, FreezeState::Stale,
                                          FreezeState::Frozen};
    int8_t slots[AudioSong::kKitSlots];
    CHECK(kitSlotsNext(s, st, 0, -1, 0, false, slots));          // ARM on the pads' kit wants no slot
    CHECK(slots[0] == 2);
    CHECK(slots[1] == -1);
    CHECK_FALSE(kitSlotsNext(s, st, 0, -1, 1u << 2, false, slots));   // its freeze keeps failing: not again
}

TEST_CASE("kit slots: the track the fingers just left keeps its engine while another will do",
          "[song][freeze][slots]") {
    AudioSong s = fourKits();
    s.kitSlots[0] = 3;                                    // just left: its notes play on to the ring's change
    s.kitSlots[1] = 2;
    const FreezeState st[AudioSong::kTracks] = {FreezeState::Frozen, FreezeState::Frozen, FreezeState::Frozen,
                                                FreezeState::Frozen};
    int8_t slots[AudioSong::kKitSlots];
    CHECK(kitSlotsNext(s, st, 1, 3, 0, true, slots));
    CHECK(slots[0] == 3);
    CHECK(slots[1] == 1);
}

TEST_CASE("kit slots: a stale track nobody plays takes a slot a frozen one only keeps cached",
          "[song][freeze][slots]") {
    AudioSong s = fourKits();
    s.armed = 0;
    s.kitSlots[0] = 1;                                    // frozen, cached while the song plays
    s.kitSlots[1] = 2;                                    // the one the fingers just left
    const FreezeState st[AudioSong::kTracks] = {FreezeState::Frozen, FreezeState::Frozen, FreezeState::Frozen,
                                                FreezeState::Stale};
    int8_t slots[AudioSong::kKitSlots];
    CHECK(kitSlotsNext(s, st, 0, 2, 0, true, slots));
    CHECK(slots[0] == 3);
    CHECK(slots[1] == 2);
}

TEST_CASE("kit slots: an empty armed kit track gets a slot for the fingers", "[song][freeze][slots]") {
    AudioSong s = fourKits();
    REQUIRE(s.addSeqTrack(4, SeqSource::Kit, 4, "/crosspad/kits/E/kit.json"));   // nothing recorded yet
    s.kitSlots[0] = 1;
    s.kitSlots[1] = 2;
    const FreezeState st[AudioSong::kTracks] = {FreezeState::Frozen, FreezeState::Frozen, FreezeState::Frozen,
                                                FreezeState::Frozen, FreezeState::None};
    int8_t slots[AudioSong::kKitSlots];
    CHECK(kitSlotsNext(s, st, 4, 1, 0, false, slots));
    CHECK((slots[0] == 4 || slots[1] == 4));
    CHECK((slots[0] == -1 || slots[1] == -1));            // the other frozen one gave its slot back
}

TEST_CASE("kit slots: a platform with one kit engine only ever fills slot 0", "[song][freeze][slots]") {
    AudioSong s = fourKits();                             // slot 1 given to track 2 when it was made
    const FreezeState st[AudioSong::kTracks] = {FreezeState::Frozen, FreezeState::Stale, FreezeState::Frozen,
                                                FreezeState::Frozen};
    int8_t slots[AudioSong::kKitSlots];
    CHECK(kitSlotsNext(s, st, 3, 1, 0, true, slots, 1));
    CHECK(slots[0] == 3);
    CHECK(slots[1] == -1);
}

TEST_CASE("kit slots: nothing to change says so", "[song][freeze][slots]") {
    AudioSong s = fourKits();
    s.kitSlots[0] = 1;
    s.kitSlots[1] = 2;
    const FreezeState st[AudioSong::kTracks] = {FreezeState::Frozen, FreezeState::Stale, FreezeState::Stale,
                                                FreezeState::Frozen};
    int8_t slots[AudioSong::kKitSlots];
    CHECK_FALSE(kitSlotsNext(s, st, 1, 2, 0, false, slots));
}

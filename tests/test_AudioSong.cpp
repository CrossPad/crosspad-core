// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include "crosspad/song/AudioSong.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace crosspad;
namespace fs = std::filesystem;

namespace {
std::string freshProject(const char* name)
{
    fs::remove_all(name);
    fs::create_directories(std::string(name) + "/" + AudioSong::clipsDir());
    return name;
}
std::string songFile(const std::string& dir) { return dir + "/" + AudioSong::fileName(); }
} // namespace

TEST_CASE("AudioSong: save and load round-trip", "[song]") {
    const auto d = freshProject("cp_song_rt");
    AudioSong a;
    a.dir = d;
    a.bpm = 96.0f;
    a.beatsPerBar = 3;
    a.length = 12;
    a.addClip(1, {"a.wav", 4, 2});
    REQUIRE(a.save());

    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    REQUIRE(b.bpm == 96.0f);
    REQUIRE(b.beatsPerBar == 3);
    REQUIRE(b.length == 12);
    REQUIRE(b.tracks[1].clips.size() == 1);
    REQUIRE(b.tracks[1].clips[0].file == "a.wav");
    REQUIRE(b.tracks[1].clips[0].bar == 4);
    REQUIRE(b.tracks[1].clips[0].bars == 2);
    REQUIRE_FALSE(fs::exists(songFile(d) + ".tmp"));
    REQUIRE_FALSE(fs::exists(songFile(d) + ".bak"));
    fs::remove_all(d);
}

TEST_CASE("AudioSong: saving twice replaces the file", "[song]") {
    const auto d = freshProject("cp_song_twice");
    AudioSong a;
    a.dir = d;
    a.bpm = 90.0f;
    REQUIRE(a.save());
    a.bpm = 140.0f;
    REQUIRE(a.save());
    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    REQUIRE(b.bpm == 140.0f);
    fs::remove_all(d);
}

TEST_CASE("AudioSong: load tells a new project from an unreadable one", "[song]") {
    const auto d = freshProject("cp_song_states");
    AudioSong s;
    REQUIRE(s.load(d) == SongLoad::New);

    { std::ofstream(songFile(d)) << "{not json"; }
    REQUIRE(s.load(d) == SongLoad::Unreadable);

    { std::ofstream(songFile(d)) << R"({"version": 999})"; }
    REQUIRE(s.load(d) == SongLoad::Unreadable);
    fs::remove_all(d);
}

TEST_CASE("AudioSong: a save cut between its renames loses nothing", "[song]") {
    const auto d = freshProject("cp_song_cut");
    AudioSong a;
    a.dir = d;
    a.bpm = 100.0f;
    REQUIRE(a.save());

    // Power lost after the old file moved aside, before the new one landed.
    fs::rename(songFile(d), songFile(d) + ".bak");
    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    REQUIRE(b.bpm == 100.0f);

    // Or with the new file complete beside it: the newer one wins.
    { std::ofstream(songFile(d) + ".tmp") << R"({"version":1,"bpm":120})"; }
    REQUIRE(b.load(d) == SongLoad::Loaded);
    REQUIRE(b.bpm == 120.0f);
    fs::remove_all(d);
}

TEST_CASE("AudioSong: a half-written tmp does not shadow the saved song", "[song]") {
    const auto d = freshProject("cp_song_halftmp");
    AudioSong a;
    a.dir = d;
    a.bpm = 88.0f;
    REQUIRE(a.save());
    { std::ofstream(songFile(d) + ".tmp") << R"({"version":1,"bp)"; }
    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    REQUIRE(b.bpm == 88.0f);
    fs::remove_all(d);
}

TEST_CASE("AudioSong: addClip returns the index of the block it added", "[song]") {
    AudioSong s;
    s.addClip(0, {"x.wav", 4, 1});
    s.addClip(0, {"y.wav", 0, 1});
    const int i = s.addClip(0, {"x.wav", 4, 2});   // same file on the same bar
    REQUIRE(i == 2);
    REQUIRE(s.tracks[0].clips[i].bars == 2);
}

TEST_CASE("AudioSong: a bar follows the metre", "[song]") {
    AudioSong s;
    s.bpm = 120.0f;
    s.beatsPerBar = 3;
    REQUIRE(s.samplesPerBar(48000) == 72000);
    s.beatsPerBar = 4;
    REQUIRE(s.samplesPerBar(48000) == 96000);
}

TEST_CASE("AudioSong: an unset tempo uses the default", "[song]") {
    AudioSong s;
    REQUIRE(s.samplesPerBar(48000) ==
            (uint32_t)(48000.0f * 60.0f / AudioSong::kDefaultBpm) * s.beatsPerBar);
}

// ── tracks a player makes (UX 2000: Arrange is where a song starts) ────────

TEST_CASE("AudioSong: a new song offers four tracks and none is in use", "[song][tracks]") {
    AudioSong s;
    REQUIRE(s.usedTracks() == 0);
    REQUIRE(s.seqTracks() == 0);
    REQUIRE(s.seqSlotFree());
    REQUIRE(s.freeTrack() == 0);
}

TEST_CASE("AudioSong: tracks are made one at a time, up to four", "[song][tracks]") {
    AudioSong s;
    for (int t = 0; t < AudioSong::kTracks; ++t) {
        REQUIRE(s.freeTrack() == t);
        REQUIRE(s.addAudioTrack(t));
    }
    REQUIRE(s.usedTracks() == AudioSong::kTracks);
    REQUIRE(s.freeTrack() == -1);
    REQUIRE_FALSE(s.addAudioTrack(0));         // already in use
}

TEST_CASE("AudioSong: one seq track at a time, and a bounce frees the slot", "[song][tracks]") {
    AudioSong s;
    REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 2));
    REQUIRE(s.seqTracks() == 1);
    REQUIRE_FALSE(s.seqSlotFree());
    REQUIRE(s.tracks[0].kind == TrackKind::Seq);
    REQUIRE(s.tracks[0].source == SeqSource::Kit);
    REQUIRE(s.tracks[0].scene == 2);

    /* The second seq is refused while the first holds the slot -- the "the
     * seq slot is taken: bounce it to free it" card. */
    REQUIRE_FALSE(s.addSeqTrack(1, SeqSource::Pitch, 0));

    const SongClip bounced{"t1_bnc.wav", 0, 4};
    REQUIRE(s.bounceSeqTrack(0, &bounced));
    REQUIRE(s.tracks[0].kind == TrackKind::Audio);
    REQUIRE(s.tracks[0].clips.size() == 1);
    REQUIRE(s.tracks[0].clips[0].file == "t1_bnc.wav");
    REQUIRE(s.seqSlotFree());
    REQUIRE(s.addSeqTrack(1, SeqSource::Pitch, 0));
}

TEST_CASE("AudioSong: only a seq track can be bounced", "[song][tracks]") {
    AudioSong s;
    REQUIRE(s.addAudioTrack(0));
    const SongClip c{"x.wav", 0, 4};
    REQUIRE_FALSE(s.bounceSeqTrack(0, &c));
    REQUIRE_FALSE(s.bounceSeqTrack(3, &c));   // not in use
}

TEST_CASE("AudioSong: removing a track closes the gap", "[song][tracks]") {
    AudioSong s;
    REQUIRE(s.addAudioTrack(0));
    REQUIRE(s.addSeqTrack(1, SeqSource::Pitch, 1));
    REQUIRE(s.addAudioTrack(2));
    s.addClip(2, {"third.wav", 0, 2});
    s.removeTrack(1);
    REQUIRE(s.usedTracks() == 2);
    REQUIRE(s.seqSlotFree());                              // the seq went with it
    REQUIRE(s.tracks[1].clips.size() == 1);
    REQUIRE(s.tracks[1].clips[0].file == "third.wav");
    REQUIRE_FALSE(s.tracks[2].inUse);
}

TEST_CASE("AudioSong: a track's kind survives a save", "[song][tracks]") {
    const auto d = freshProject("cp_song_kinds");
    AudioSong a;
    a.dir = d;
    REQUIRE(a.addAudioTrack(0));
    a.addClip(0, {"beat.wav", 0, 4});
    REQUIRE(a.addSeqTrack(1, SeqSource::Pitch, 3));
    REQUIRE(a.save());

    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    REQUIRE(b.usedTracks() == 2);
    REQUIRE(b.tracks[0].kind == TrackKind::Audio);
    REQUIRE(b.tracks[1].kind == TrackKind::Seq);
    REQUIRE(b.tracks[1].source == SeqSource::Pitch);
    REQUIRE(b.tracks[1].scene == 3);
    REQUIRE_FALSE(b.seqSlotFree());
    fs::remove_all(d);
}

TEST_CASE("AudioSong: a song written before tracks had kinds counts its clips", "[song][tracks]") {
    const auto d = freshProject("cp_song_legacy");
    {
        /* No "used" and no "kind": what every song on a card holds today. */
        std::ofstream(songFile(d)) << R"({"version":1,"bpm":90,"tracks":[
            {"volume":1,"muted":false,"clips":[{"file":"a.wav","bar":0,"bars":4}]},
            {"volume":1,"muted":false,"clips":[]},
            {"volume":1,"muted":false,"clips":[{"file":"b.wav","bar":0,"bars":2}]},
            {"volume":1,"muted":false,"clips":[]}]})";
    }
    AudioSong s;
    REQUIRE(s.load(d) == SongLoad::Loaded);
    REQUIRE(s.tracks[0].inUse);
    REQUIRE_FALSE(s.tracks[1].inUse);
    REQUIRE(s.tracks[2].inUse);
    REQUIRE(s.usedTracks() == 2);
    REQUIRE(s.seqTracks() == 0);
    fs::remove_all(d);
}

TEST_CASE("AudioSong: a file claiming two seq tracks keeps the first", "[song][tracks]") {
    const auto d = freshProject("cp_song_twoseq");
    {
        std::ofstream(songFile(d)) << R"({"version":1,"tracks":[
            {"used":true,"kind":1,"src":0,"scene":0,"clips":[]},
            {"used":true,"kind":1,"src":1,"scene":1,"clips":[]}]})";
    }
    AudioSong s;
    REQUIRE(s.load(d) == SongLoad::Loaded);
    REQUIRE(s.seqTracks() == 1);
    REQUIRE(s.tracks[0].kind == TrackKind::Seq);
    REQUIRE(s.tracks[1].kind == TrackKind::Audio);
    fs::remove_all(d);
}

TEST_CASE("AudioSong: a bounce whose block is already placed keeps it", "[song][tracks]") {
    /* ISongEngine::bounceScene() writes the WAV and puts its block on the
     * track itself; Arrange then only has to say the seq is now audio. */
    AudioSong s;
    REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
    s.addClip(0, {"placed_by_engine.wav", 0, 4});
    REQUIRE(s.bounceSeqTrack(0));
    REQUIRE(s.tracks[0].kind == TrackKind::Audio);
    REQUIRE(s.tracks[0].clips.size() == 1);
    REQUIRE(s.tracks[0].clips[0].file == "placed_by_engine.wav");
    REQUIRE(s.seqSlotFree());
}

// ── editing by bars ────────────────────────────────────────────────────────

namespace {
constexpr uint32_t kSr = 48000;
constexpr uint32_t kSpb = 96000;   // a bar at 120 BPM, 4/4, 48 kHz
AudioSong songAt120()
{
    AudioSong s;
    s.bpm = 120.0f;
    return s;
}
} // namespace

TEST_CASE("AudioSong: a cut splits an audio block where it falls", "[song][cut]") {
    AudioSong s = songAt120();
    REQUIRE(s.samplesPerBar(kSr) == kSpb);
    s.addClip(0, {"a.wav", 2, 4});
    REQUIRE(s.cutClip(0, 4, kSpb) == 1);
    const auto& cs = s.tracks[0].clips;
    REQUIRE(cs.size() == 2);
    REQUIRE(cs[0].bar == 2);
    REQUIRE(cs[0].bars == 2);
    REQUIRE(cs[0].offset == 0);
    REQUIRE(cs[0].frames == 2 * kSpb);   // stops at the cut
    REQUIRE(cs[1].file == "a.wav");
    REQUIRE(cs[1].bar == 4);
    REQUIRE(cs[1].bars == 2);
    REQUIRE(cs[1].offset == 2 * kSpb);   // plays on from it
    REQUIRE(cs[1].frames == 0);
}

TEST_CASE("AudioSong: a cut on a block's first bar or outside it does nothing", "[song][cut]") {
    AudioSong s = songAt120();
    s.addClip(0, {"a.wav", 2, 4});
    REQUIRE(s.cutClip(0, 2, kSpb) == -1);
    REQUIRE(s.cutClip(0, 6, kSpb) == -1);
    REQUIRE(s.cutClip(0, 1, kSpb) == -1);
    REQUIRE(s.tracks[0].clips.size() == 1);
}

TEST_CASE("AudioSong: a cut goes by what sounds, not by the stored length", "[song][cut]") {
    AudioSong s = songAt120();
    s.addClip(0, {"a.wav", 0, 8});
    s.addClip(0, {"b.wav", 4, 2});   // a sounds 0..4
    REQUIRE(s.cutClip(0, 5, kSpb) == 2);
    REQUIRE(s.tracks[0].clips[1].file == "b.wav");
    REQUIRE(s.tracks[0].clips[1].bars == 1);
    REQUIRE(s.tracks[0].clips[2].file == "b.wav");
    REQUIRE(s.tracks[0].clips[2].offset == kSpb);
}

TEST_CASE("AudioSong: a seq block is cut by scene bars", "[song][cut][seq]") {
    AudioSong s = songAt120();
    REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 0));
    REQUIRE(s.seqBlocksExplicit(8));
    REQUIRE(s.tracks[1].seqBlocks);
    REQUIRE(s.cutClip(1, 3, kSpb) == 1);
    const auto& cs = s.tracks[1].clips;
    REQUIRE(cs[0].bar == 0);
    REQUIRE(cs[0].bars == 3);
    REQUIRE(cs[0].from == 0);
    REQUIRE(cs[1].bar == 3);
    REQUIRE(cs[1].bars == 5);
    REQUIRE(cs[1].from == 3);
    REQUIRE(cs[1].file.empty());
    REQUIRE(cs[1].offset == 0);
}

TEST_CASE("AudioSong: inserting bars cuts what crosses and moves the rest", "[song][cut]") {
    AudioSong s = songAt120();
    s.length = 8;
    s.addClip(0, {"a.wav", 0, 4});
    s.addClip(0, {"b.wav", 4, 4});
    s.insertBars(2, 4, kSpb);
    const auto& cs = s.tracks[0].clips;
    REQUIRE(cs.size() == 3);
    REQUIRE(cs[0].bar == 0);
    REQUIRE(cs[0].bars == 2);
    REQUIRE(cs[1].file == "a.wav");
    REQUIRE(cs[1].bar == 6);
    REQUIRE(cs[1].offset == 2 * kSpb);
    REQUIRE(cs[2].file == "b.wav");
    REQUIRE(cs[2].bar == 8);
    REQUIRE(s.length == 12);
}

TEST_CASE("AudioSong: a region doubled doubles the seq by bars as well", "[song][cut][seq]") {
    AudioSong s = songAt120();
    s.addClip(0, {"a.wav", 0, 8});
    REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 0));
    s.seqBlocksExplicit(8);
    /* Region x2 over bars 0..4, as Arrange does it. */
    const auto part = s.copyRange(0, 4, kSpb);
    REQUIRE(part.size() == 2);
    s.insertBars(4, 4, kSpb);
    s.pasteRange(part, 4);

    const auto& a = s.tracks[0].clips;
    REQUIRE(a.size() == 3);
    REQUIRE(a[0].bar == 0);
    REQUIRE(a[0].frames == 4 * kSpb);
    REQUIRE(a[1].bar == 4);
    REQUIRE(a[1].offset == 0);           // the copy: bars 0..4 again
    REQUIRE(a[1].frames == 4 * kSpb);
    REQUIRE(a[2].bar == 8);
    REQUIRE(a[2].offset == 4 * kSpb);    // the rest of the file after it

    const auto& q = s.tracks[1].clips;
    REQUIRE(q.size() == 3);
    REQUIRE((q[0].bar == 0 && q[0].bars == 4 && q[0].from == 0));
    REQUIRE((q[1].bar == 4 && q[1].bars == 4 && q[1].from == 0));
    REQUIRE((q[2].bar == 8 && q[2].bars == 4 && q[2].from == 4));
}

TEST_CASE("AudioSong: a block copied onto a track of the other kind is left out", "[song][cut][seq]") {
    AudioSong s = songAt120();
    std::vector<RangeClip> part;
    part.push_back(RangeClip{0, 0, SongClip{"a.wav", 0, 2}});
    part.push_back(RangeClip{1, 0, SongClip{"", 0, 2}});
    REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
    s.pasteRange(part, 4);
    REQUIRE(s.tracks[0].clips.empty());
    REQUIRE(s.tracks[1].clips.empty());
}

TEST_CASE("AudioSong: cuts and seq blocks survive a save", "[song][cut][seq]") {
    const auto d = freshProject("cp_song_cut");
    AudioSong a = songAt120();
    a.dir = d;
    a.addClip(0, {"a.wav", 0, 4});
    a.cutClip(0, 1, kSpb);
    REQUIRE(a.addSeqTrack(1, SeqSource::Pitch, 2));
    a.seqBlocksExplicit(6);
    a.cutClip(1, 2, kSpb);
    REQUIRE(a.save());

    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    REQUIRE(b.tracks[0].clips.size() == 2);
    REQUIRE(b.tracks[0].clips[0].frames == kSpb);
    REQUIRE(b.tracks[0].clips[1].offset == kSpb);
    REQUIRE(b.tracks[1].kind == TrackKind::Seq);
    REQUIRE(b.tracks[1].seqBlocks);
    REQUIRE(b.tracks[1].clips.size() == 2);
    REQUIRE(b.tracks[1].clips[1].from == 2);
    REQUIRE(b.tracks[1].clips[1].file.empty());
}

TEST_CASE("AudioSong: a bounce leaves no seq block behind", "[song][seq]") {
    AudioSong s = songAt120();
    REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
    s.seqBlocksExplicit(4);
    s.addClip(0, {"bounce.wav", 0, 4});
    REQUIRE(s.bounceSeqTrack(0));
    REQUIRE(s.tracks[0].kind == TrackKind::Audio);
    REQUIRE_FALSE(s.tracks[0].seqBlocks);
    REQUIRE(s.tracks[0].clips.size() == 1);
    REQUIRE(s.tracks[0].clips[0].file == "bounce.wav");
}

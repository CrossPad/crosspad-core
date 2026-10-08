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

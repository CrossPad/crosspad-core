// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "crosspad/song/AudioSong.hpp"
#include "crosspad/song/SeqFreezeKey.hpp"

#include <filesystem>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

using namespace crosspad;
namespace fs = std::filesystem;

namespace {
std::string freshProject(const char* name)
{
    const std::string dir = (fs::temp_directory_path() / "cp_core_tests" / name).string();
    fs::remove_all(dir);
    fs::create_directories(dir + "/" + AudioSong::clipsDir());
    return dir;
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

TEST_CASE("AudioSong: a new seq track is empty, whatever it plays through", "[song][tracks]") {
    AudioSong s;
    REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
    REQUIRE(s.addSeqTrack(1, SeqSource::Pitch, 1));
    REQUIRE(s.addSeqTrack(2, SeqSource::Kit, 2, "/crosspad/kits/B/kit.json"));
    for (int t = 0; t < 3; ++t) {
        CHECK(s.tracks[t].seqBlocks);       // no scene over the whole song
        CHECK(s.tracks[t].clips.empty());
    }
    CHECK_FALSE(s.seqBlocksExplicit(8));     // nothing to give blocks to
    CHECK(s.playEnd() == 0);
}

TEST_CASE("AudioSong: kit tracks each keep a kit and take turns on the kits' engine; a bounce frees a slot", "[song][tracks]") {
    AudioSong s;
    s.kit = "/crosspad/kits/A/kit.json";
    REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 2));
    REQUIRE(s.seqTracks() == 1);
    REQUIRE(s.tracks[0].kind == TrackKind::Seq);
    REQUIRE(s.tracks[0].source == SeqSource::Kit);
    REQUIRE(s.tracks[0].scene == 2);
    REQUIRE(s.seqRoute(0) == 0);
    REQUIRE(s.tracks[0].kit == "/crosspad/kits/A/kit.json");  // none given: the song's
    REQUIRE(s.liveKit == 0);                               // the first kit track has the engine
    REQUIRE(s.seqSlotFree());

    /* A second one keeps its own kit and waits for the engine. */
    REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 3, "/crosspad/kits/B/kit.json"));
    REQUIRE(s.seqRoute(1) == 0);
    REQUIRE(s.tracks[1].kit == "/crosspad/kits/B/kit.json");
    REQUIRE(s.tracks[1].seqBlocks);
    REQUIRE(s.seqTracks() == 2);
    REQUIRE(s.liveKit == 0);
    REQUIRE(s.liveRoute(1) == -1);
    REQUIRE(s.kit == "/crosspad/kits/A/kit.json");

    /* Given the engine, its kit is the song's -- what the pads play and the
     * Sampler edits -- and a kit loaded meanwhile becomes its own. */
    REQUIRE(s.setLiveKit(1));
    REQUIRE(s.liveRoute(0) == -1);
    REQUIRE(s.liveRoute(1) == 0);
    REQUIRE(s.seqTrackOnRoute(0) == 1);
    REQUIRE(s.kit == "/crosspad/kits/B/kit.json");
    s.kitHeld("/crosspad/kits/B2/kit.json");
    REQUIRE(s.tracks[1].kit == "/crosspad/kits/B2/kit.json");
    REQUIRE(s.tracks[0].kit == "/crosspad/kits/A/kit.json");
    /* A kit picked for a track: the song's too only while it is live. */
    s.setTrackKit(0, "/crosspad/kits/A2/kit.json");
    REQUIRE(s.kit == "/crosspad/kits/B2/kit.json");
    s.setTrackKit(1, "/crosspad/kits/B3/kit.json");
    REQUIRE(s.kit == "/crosspad/kits/B3/kit.json");
    REQUIRE_FALSE(s.setLiveKit(5));                        // no kit track

    const SongClip bounced{"t1_bnc.wav", 0, 4};
    REQUIRE(s.bounceSeqTrack(1, &bounced));
    REQUIRE(s.tracks[1].kind == TrackKind::Audio);
    REQUIRE(s.tracks[1].clips.size() == 1);
    REQUIRE(s.tracks[1].clips[0].file == "t1_bnc.wav");
    REQUIRE(s.liveKit == -1);                              // the engine is free for the next
    REQUIRE(s.seqSlotFree());
    REQUIRE(s.freeSeqRoute(SeqSource::Kit) == 0);
    /* A new kit track with nobody live takes the engine, and its kit is the song's. */
    REQUIRE(s.addSeqTrack(3, SeqSource::Kit, 5, "/crosspad/kits/D/kit.json"));
    REQUIRE(s.liveKit == 3);
    REQUIRE(s.kit == "/crosspad/kits/D/kit.json");
}

TEST_CASE("AudioSong: Drums, Slice and Pitch tracks at once, a route each, and audio beside them", "[song][tracks][routes]") {
    AudioSong s;
    REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0, "/crosspad/kits/DRUMS/kit.json"));
    REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 1, "/crosspad/kits/SLICE/kit.json"));
    REQUIRE(s.freeSeqRoute(SeqSource::Pitch) == 2);
    REQUIRE(s.addSeqTrack(2, SeqSource::Pitch, 2));
    REQUIRE(s.addAudioTrack(3));
    CHECK(s.seqRoute(0) == 0);
    CHECK(s.seqRoute(1) == 0);
    CHECK(s.seqRoute(2) == 2);
    CHECK(s.seqRoute(3) == -1);
    CHECK(s.liveRoute(1) == -1);                           // the Drums track has the kits' engine
    CHECK(s.seqTrackOnRoute(2) == 2);
    CHECK(s.seqTracks() == 3);
    CHECK(s.kit == "/crosspad/kits/DRUMS/kit.json");
    /* No second Pitch; a third kit waits its turn on the kits' engine. */
    CHECK(s.freeSeqRoute(SeqSource::Pitch) == -1);
    CHECK(s.freeSeqRoute(SeqSource::Kit) == 0);
    /* A Pitch track first leaves the kits' route free. */
    AudioSong p;
    REQUIRE(p.addSeqTrack(0, SeqSource::Pitch, 0));
    CHECK(p.seqRoute(0) == 2);
    CHECK(p.freeSeqRoute(SeqSource::Kit) == 0);
    REQUIRE(p.addSeqTrack(1, SeqSource::Kit, 1));
    CHECK(p.seqRoute(1) == 0);
}

TEST_CASE("AudioSong: a Pitch track beside kit tracks survives a save", "[song][tracks][routes]") {
    const auto d = freshProject("cp_song_four_sources");
    AudioSong a;
    a.dir = d;
    a.bpm = 120.0f;
    REQUIRE(a.addSeqTrack(0, SeqSource::Kit, 0, "/crosspad/kits/DRUMS/kit.json"));
    REQUIRE(a.addSeqTrack(1, SeqSource::Kit, 1, "/crosspad/kits/SLICE/kit.json"));
    REQUIRE(a.addSeqTrack(2, SeqSource::Pitch, 2));
    REQUIRE(a.save());
    {
        std::ifstream in(d + "/song.json");
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text.find("\"version\":6") != std::string::npos);   // two kit tracks: 6
    }
    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    CHECK(b.seqRoute(0) == 0);
    CHECK(b.seqRoute(1) == 0);
    CHECK(b.seqRoute(2) == 2);
    CHECK(b.tracks[1].kit == "/crosspad/kits/SLICE/kit.json");
}

TEST_CASE("AudioSong: kit tracks take turns on the kits' engine", "[song][tracks][routes]") {
    const auto d = freshProject("cp_song_kit_slot");
    AudioSong a;
    a.dir = d;
    a.bpm = 120.0f;
    for (int t = 0; t < 5; ++t) {
        const std::string kit = "/crosspad/kits/K" + std::to_string(t) + "/kit.json";
        REQUIRE(a.addSeqTrack(t, SeqSource::Kit, (uint8_t)t, kit));
        CHECK(a.seqRoute(t) == 0);
    }
    /* One plays through the engine, the rest have nothing to play them. */
    CHECK(a.liveKit == 0);
    CHECK(a.seqTrackOnRoute(0) == 0);
    CHECK(a.liveRoute(0) == 0);
    CHECK(a.liveRoute(3) == -1);
    REQUIRE(a.setLiveKit(3));
    CHECK(a.seqTrackOnRoute(0) == 3);
    CHECK(a.liveRoute(0) == -1);
    CHECK(a.kit == "/crosspad/kits/K3/kit.json");
    /* A track above it removed: the engine moves up with its track. */
    a.removeTrack(1);
    CHECK(a.liveKit == 2);
    CHECK(a.tracks[2].kit == "/crosspad/kits/K3/kit.json");
    a.removeTrack(2);
    CHECK(a.liveKit == -1);
    CHECK(a.seqTrackOnRoute(0) == -1);
    /* More than one kit is version 6; the armed one has the engine when the
     * song opens, and its kit is the song's. */
    a.armed = 2;
    REQUIRE(a.save());
    {
        std::ifstream in(d + "/song.json");
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text.find("\"version\":6") != std::string::npos);
    }
    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    CHECK(b.seqTracks() == 3);
    CHECK(b.seqRoute(1) == 0);
    CHECK(b.liveKit == 2);
    CHECK(b.kit == b.tracks[2].kit);
    /* A seq track is a scene: eight at most. */
    AudioSong c;
    for (int t = 0; t < AudioSong::kSeqScenes; ++t)
        REQUIRE(c.addSeqTrack(t, SeqSource::Kit, (uint8_t)t, "/crosspad/kits/K/kit.json"));
    CHECK_FALSE(c.seqSlotFree());
}

TEST_CASE("AudioSong: every seq track has a scene of its own", "[song][tracks][scenes]") {
    AudioSong s;
    REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 1, "/crosspad/kits/DRUMS/kit.json"));
    /* The Slice track asked for the Drums track's scene: refused, or its seq
     * view would clear the Drums notes and keep them as its own. */
    CHECK_FALSE(s.addSeqTrack(1, SeqSource::Kit, 1, "/crosspad/kits/SLICE/kit.json"));
    CHECK(s.sceneUsed(1));
    CHECK_FALSE(s.sceneUsed(1, 0));                        // but for itself
    /* The scene just added is the last one, whatever the sequencer says is
     * active while it plays. */
    CHECK(s.sceneForNewSeq(3, true) == 2);
    /* None added (all eight there): the lowest no track plays. */
    CHECK(s.sceneForNewSeq(3, false) == 0);
    REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 2, "/crosspad/kits/SLICE/kit.json"));
    REQUIRE(s.addSeqTrack(2, SeqSource::Pitch, 0));
    CHECK(s.sceneForNewSeq(3, false) == -1);
    CHECK(s.sceneForNewSeq(3, true) == -1);                // the last is taken too
}

TEST_CASE("AudioSong: the armed track is a seq track, else the first", "[song][tracks]") {
    AudioSong s;
    REQUIRE(s.armedSeqTrack() == -1);
    REQUIRE(s.addAudioTrack(0));
    REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 0));
    REQUIRE(s.addSeqTrack(2, SeqSource::Kit, 1, "/crosspad/kits/B/kit.json"));
    REQUIRE(s.armedSeqTrack() == 1);
    s.armed = 2;
    REQUIRE(s.armedSeqTrack() == 2);
    s.armed = 0;                                           // an audio track takes no take
    REQUIRE(s.armedSeqTrack() == 1);
    REQUIRE(s.seqTrackOnRoute(0) == 1);                    // the first kit track has the engine
}

TEST_CASE("AudioSong: a song from before every kit track had a kit opens with the song's on the first", "[song][tracks]") {
    const auto d = freshProject("cp_song_two_kits");
    {
        /* Version 3: the first kit track played the song's kit, the second
         * its own on the second kit engine, and that one was armed. */
        std::ofstream out(d + "/song.json");
        out << R"({"version":3,"bpm":120,"kit":"/crosspad/kits/A/kit.json","armed":1,"tracks":[)"
               R"({"used":true,"kind":1,"src":0,"scene":0,"clips":[]},)"
               R"({"used":true,"kind":1,"src":0,"scene":1,"kit":"/crosspad/kits/B/kit.json","clips":[]}]})";
    }
    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    REQUIRE(b.seqTracks() == 2);
    REQUIRE(b.tracks[0].kit == "/crosspad/kits/A/kit.json");
    REQUIRE(b.tracks[1].kit == "/crosspad/kits/B/kit.json");
    REQUIRE(b.seqRoute(0) == 0);
    REQUIRE(b.seqRoute(1) == 0);
    REQUIRE(b.armed == 1);
    REQUIRE(b.liveKit == 1);                               // the armed one plays live
    REQUIRE(b.kit == "/crosspad/kits/B/kit.json");
    fs::remove_all(d);
}

TEST_CASE("AudioSong: a file with two Pitch tracks keeps the first", "[song][tracks]") {
    const auto d = freshProject("cp_song_too_many_seqs");
    {
        std::ofstream out(d + "/song.json");
        out << R"({"version":3,"bpm":120,"tracks":[)"
               R"({"used":true,"kind":1,"src":0,"scene":0,"clips":[]},)"
               R"({"used":true,"kind":1,"src":1,"scene":1,"clips":[]},)"
               R"({"used":true,"kind":1,"src":0,"scene":2,"kit":"/crosspad/kits/B/kit.json","clips":[]},)"
               R"({"used":true,"kind":1,"src":1,"scene":3,"clips":[]}]})";
    }
    AudioSong s;
    REQUIRE(s.load(d) == SongLoad::Loaded);
    REQUIRE(s.seqRoute(0) == 0);
    REQUIRE(s.seqRoute(1) == 2);
    REQUIRE(s.seqRoute(2) == 0);
    REQUIRE(s.tracks[3].kind == TrackKind::Audio);         // a second Pitch track
    fs::remove_all(d);
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
    REQUIRE(b.seqSlotFree());                              // a kit track beside the Pitch one
    REQUIRE(b.freeSeqRoute(SeqSource::Pitch) == -1);
    fs::remove_all(d);
}

TEST_CASE("AudioSong: how a track follows the tempo survives a save", "[song][tracks][stretch]") {
    const auto d = freshProject("cp_song_stretch");
    {
        /* What 1.57 first wrote: true, for a stretch that keeps the pitch. */
        std::ofstream(songFile(d)) << R"({"version":1,"bpm":90,"tracks":[
            {"used":true,"stretch":true,"clips":[{"file":"a.wav","bar":0,"bars":4}]},
            {"used":true,"stretch":"repitch","clips":[{"file":"b.wav","bar":0,"bars":2}]},
            {"used":true,"clips":[]},
            {"used":true,"kind":1,"scene":2,"stretch":true,"clips":[]}]})";
    }
    AudioSong a;
    REQUIRE(a.load(d) == SongLoad::Loaded);
    CHECK(a.tracks[0].stretch == StretchMode::KeepPitch);
    CHECK(a.tracks[1].stretch == StretchMode::Repitch);
    CHECK(a.tracks[2].stretch == StretchMode::Off);
    CHECK(a.tracks[3].stretch == StretchMode::Off);   // a seq track has no audio to fit
    REQUIRE(a.save());

    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    CHECK(b.tracks[0].stretch == StretchMode::KeepPitch);
    CHECK(b.tracks[1].stretch == StretchMode::Repitch);
    CHECK(b.tracks[2].stretch == StretchMode::Off);
    fs::remove_all(d);
}

TEST_CASE("AudioSong: a track's name and solo survive a save", "[song][tracks]") {
    const auto d = freshProject("cp_song_names");
    AudioSong a;
    a.dir = d;
    REQUIRE(a.addAudioTrack(0));
    REQUIRE(a.addAudioTrack(1));
    REQUIRE(a.addAudioTrack(2));
    a.tracks[0].name = "Drums";
    a.tracks[1].solo = true;
    a.tracks[2].muted = true;
    CHECK(a.silenced(0));          // another track is soloed
    CHECK_FALSE(a.silenced(1));
    CHECK(a.silenced(2));
    REQUIRE(a.save());

    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    CHECK(b.tracks[0].name == "Drums");
    CHECK(b.tracks[1].name.empty());
    CHECK(b.tracks[1].solo);
    CHECK_FALSE(b.tracks[0].solo);
    b.tracks[1].solo = false;
    CHECK_FALSE(b.silenced(0));    // nothing soloed: only the mute counts
    CHECK(b.silenced(2));
    fs::remove_all(d);
}

TEST_CASE("AudioSong: a track's pan survives a save, centre left out", "[song][tracks]") {
    const auto d = freshProject("cp_song_pan");
    AudioSong a;
    a.dir = d;
    REQUIRE(a.addAudioTrack(0));
    REQUIRE(a.addAudioTrack(1));
    a.tracks[0].pan = -0.4f;
    REQUIRE(a.save());

    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    CHECK(b.tracks[0].pan == -0.4f);
    CHECK(b.tracks[1].pan == 0.0f);
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

TEST_CASE("AudioSong: a freeze as 2.9 wrote it -- one file whole -- loads as one block", "[song][freeze]") {
    const auto d = freshProject("cp_song_freeze29");
    {
        std::ofstream(songFile(d)) << R"({"version":3,"bpm":120,"tracks":[
            {"volume":1,"muted":false,"used":true,"kind":1,"src":0,"scene":1,"blocks":true,
             "freeze":{"file":"FRZ1_01.wav","bar":4,"bars":8,"key":77},
             "clips":[{"bar":4,"bars":8,"notes":{"bars":1,"ev":"0,1,100,0"}}]},
            {"volume":1,"muted":false,"clips":[]},
            {"volume":1,"muted":false,"clips":[]},
            {"volume":1,"muted":false,"clips":[]}]})";
    }
    AudioSong s;
    REQUIRE(s.load(d) == SongLoad::Loaded);
    REQUIRE(s.tracks[0].freeze.size() == 1);
    CHECK(s.tracks[0].freeze[0].file == "FRZ1_01.wav");
    CHECK(s.tracks[0].freeze[0].bar == 4);
    CHECK(s.tracks[0].freeze[0].bars == 8);
    CHECK(s.tracks[0].freeze[0].offset == 0);
    CHECK(s.tracks[0].freezeKey == 77);
    /* Written back as blocks, and read the same. */
    s.tracks[0].freeze.push_back({"FRZ1_01.wav", 12, 2, 88200});
    REQUIRE(s.save());
    AudioSong back;
    REQUIRE(back.load(d) == SongLoad::Loaded);
    REQUIRE(back.tracks[0].freeze.size() == 2);
    CHECK(back.tracks[0].freeze[1].bar == 12);
    CHECK(back.tracks[0].freeze[1].offset == 88200);
    fs::remove_all(d);
}

TEST_CASE("AudioSong: a file with two kit tracks on the song's kit keeps both, the first live", "[song][tracks]") {
    const auto d = freshProject("cp_song_twoseq");
    {
        std::ofstream(songFile(d)) << R"({"version":1,"kit":"/crosspad/kits/A/kit.json","tracks":[
            {"used":true,"kind":1,"src":0,"scene":0,"clips":[]},
            {"used":true,"kind":1,"src":0,"scene":1,"clips":[]}]})";
    }
    AudioSong s;
    REQUIRE(s.load(d) == SongLoad::Loaded);
    REQUIRE(s.seqTracks() == 2);
    REQUIRE(s.tracks[1].kit == "/crosspad/kits/A/kit.json");
    REQUIRE(s.liveKit == 0);
    REQUIRE(s.liveRoute(1) == -1);
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

TEST_CASE("audio song: a cut moves an element clip's from and keeps its pass", "[song][cut][element]") {
    AudioSong s = songAt120();
    SongClip c;
    c.element = ".elements/E00000001";
    c.file = "B0000000000000001.wav";
    c.bar = 2;
    c.bars = 6;
    c.from = 1;
    c.frames = 3 * kSpb;
    s.addClip(0, c);
    REQUIRE(s.cutClip(0, 4, kSpb) == 1);
    const auto& cs = s.tracks[0].clips;
    REQUIRE(cs.size() == 2);
    CHECK((cs[0].bar == 2 && cs[0].bars == 2 && cs[0].from == 1));
    CHECK((cs[1].bar == 4 && cs[1].bars == 4 && cs[1].from == 3));
    for (const auto& k : cs) {
        CHECK(k.frames == 3 * kSpb);   // one pass of the element, whatever the cut
        CHECK(k.offset == 0);
        CHECK(k.element == c.element);
        CHECK(k.file == c.file);
    }
}

TEST_CASE("audio song: a pasted unbaked element clip lands on an audio track", "[song][cut][element]") {
    AudioSong s = songAt120();
    SongClip c;
    c.element = ".elements/E00000001";
    c.bars = 2;
    std::vector<RangeClip> part;
    part.push_back(RangeClip{0, 0, c});
    part.push_back(RangeClip{1, 0, c});
    REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 0));
    s.pasteRange(part, 4);
    REQUIRE(s.tracks[0].clips.size() == 1);
    CHECK(s.tracks[0].clips[0].bar == 4);
    CHECK(s.tracks[0].clips[0].isElement());
    CHECK(s.tracks[1].clips.empty());
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
    s.tracks[1].seqBlocks = false;   // from before blocks had notes
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

TEST_CASE("AudioSong: two tracks change places, and what names them follows", "[song][tracks]") {
    AudioSong s = songAt120();
    s.addClip(0, {"a.wav", 0, 4});
    s.tracks[0].inUse = true;
    REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 0, "/crosspad/kits/B/kit.json"));
    s.tracks[1].name = "B";
    s.tracks[0].name = "A";
    s.armed = 1;
    s.liveKit = 1;
    ClipSet set;
    set.setRows(1);
    set.put(0, 0, "a.wav");
    set.put(0, 1, "b.wav");
    s.sets.push_back(set);

    REQUIRE(s.swapTracks(0, 1));
    CHECK(s.tracks[0].name == "B");
    CHECK(s.tracks[1].name == "A");
    CHECK(s.tracks[1].clips.size() == 1);
    CHECK(s.armed == 0);
    CHECK(s.liveKit == 0);
    CHECK(s.sets[0].at(0, 0) == "b.wav");
    CHECK(s.sets[0].at(0, 1) == "a.wav");
    CHECK_FALSE(s.swapTracks(1, 2));        // track 3 is not in use
}

TEST_CASE("AudioSong: deleting bars takes them out and closes the gap", "[song][cut]") {
    AudioSong s = songAt120();
    s.length = 12;
    s.addClip(0, {"a.wav", 0, 4});
    s.addClip(0, {"b.wav", 4, 4});
    s.addClip(0, {"c.wav", 8, 4});
    s.loopStart = 8;
    s.loopEnd = 12;
    s.deleteBars(2, 6, kSpb);
    const auto& cs = s.tracks[0].clips;
    REQUIRE(cs.size() == 3);
    CHECK(cs[0].file == "a.wav");                 // bars 0..2 of it stay
    CHECK(cs[0].bar == 0);
    CHECK(cs[0].bars == 2);
    CHECK(cs[1].file == "b.wav");                 // what of b was past bar 6, from bar 2
    CHECK(cs[1].bar == 2);
    CHECK(cs[1].bars == 2);
    CHECK(cs[1].offset == 2 * kSpb);
    CHECK(cs[2].file == "c.wav");
    CHECK(cs[2].bar == 4);
    CHECK(s.length == 8);
    CHECK(s.loopStart == 4);
    CHECK(s.loopEnd == 8);

    s.deleteBars(3, 9, kSpb);                     // the region taken with it
    CHECK_FALSE(s.hasRegion());
    CHECK(s.length == 3);
}

TEST_CASE("AudioSong: a region doubled doubles the seq by bars as well", "[song][cut][seq]") {
    AudioSong s = songAt120();
    s.addClip(0, {"a.wav", 0, 8});
    REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 0));
    s.tracks[1].seqBlocks = false;   // from before blocks had notes
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
    a.tracks[1].seqBlocks = false;   // from before blocks had notes
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

// ── a seq block's own notes ────────────────────────────────────────────────

namespace {
SeqNote note(uint32_t tick, uint8_t pad, uint8_t vel = 100, uint32_t len = 0, uint8_t perf = 0)
{
    SeqNote n;
    n.tick = tick;
    n.pad = pad;
    n.vel = vel;
    n.len = len;
    n.perf = perf;
    return n;
}
constexpr uint32_t kTpb = AudioSong::kSeqPpq * 4;   // ticks a bar in 4/4
} // namespace

TEST_CASE("AudioSong: a block's own notes survive a save, and only they make it a 2", "[song][seq][notes]") {
    const auto d = freshProject("cp_song_notes");
    AudioSong a = songAt120();
    a.dir = d;
    REQUIRE(a.addSeqTrack(0, SeqSource::Kit, 0));
    REQUIRE(a.save());
    {
        std::ifstream f(songFile(d));
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(text.find("\"version\":1") != std::string::npos);   // still readable by older firmware
    }
    REQUIRE(a.putSeqTake(0, 2, makeSeqPattern(2, {note(96, 3, 90, 48), note(0, 1), note(400, 15, 127, 0, 0x21)}), kSpb) >= 0);
    REQUIRE(a.save());
    {
        std::ifstream f(songFile(d));
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(text.find("\"version\":2") != std::string::npos);
    }

    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    REQUIRE(b.tracks[0].clips.size() == 1);
    const SongClip& c = b.tracks[0].clips[0];
    REQUIRE(c.bar == 2);
    REQUIRE(c.bars == 2);
    REQUIRE(c.notes);
    REQUIRE(c.notes->bars == 2);
    REQUIRE(c.notes->notes.size() == 3);
    /* Sorted by tick, every field as it was. */
    CHECK(c.notes->notes[0].tick == 0);
    CHECK(c.notes->notes[0].pad == 1);
    CHECK(c.notes->notes[1].tick == 96);
    CHECK(c.notes->notes[1].len == 48);
    CHECK(c.notes->notes[1].vel == 90);
    CHECK(c.notes->notes[2].pad == 15);
    CHECK(c.notes->notes[2].perf == 0x21);
    fs::remove_all(d);
}

TEST_CASE("AudioSong: the Pitch track's pad map survives a save and is in its freeze key", "[song][pitchpads]") {
    const auto d = freshProject("cp_song_pitchpads");
    AudioSong a = songAt120();
    a.dir = d;
    REQUIRE(a.addSeqTrack(0, SeqSource::Pitch, 0));
    a.instrument = "/crosspad/pitchplay/sub/instrument.json";
    const uint32_t keyWithout = seq_freeze_key(a, 0, 44100, SceneLookup{});
    a.pitchPads.set = true;
    a.pitchPads.root = 5;
    a.pitchPads.octave = -1;
    a.pitchPads.scale = 2;
    a.pitchPads.transpose = -3;
    a.pitchPads.chords = true;
    a.pitchPads.silent = 0x8001;
    for (int i = 0; i < 16; ++i) {
        a.pitchPads.note[i] = (uint8_t)(30 + i);
        a.pitchPads.count[i] = 1;
        for (int c = 0; c < 6; ++c) a.pitchPads.chord[i][c] = c ? 0xFF : (uint8_t)(30 + i);
    }
    const uint32_t keyWith = seq_freeze_key(a, 0, 44100, SceneLookup{});
    CHECK(keyWith != keyWithout);
    REQUIRE(a.save());

    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    CHECK(b.pitchPads == a.pitchPads);
    CHECK(seq_freeze_key(b, 0, 44100, SceneLookup{}) == keyWith);
    b.pitchPads.note[3] += 12;                           // that pad an octave up: another freeze
    CHECK(seq_freeze_key(b, 0, 44100, SceneLookup{}) != keyWith);
    fs::remove_all(d);
}

TEST_CASE("AudioSong: a track's swing -- its scene's -- is in its freeze key, straight left out", "[song][swing]") {
    AudioSong a = songAt120();
    REQUIRE(a.addSeqTrack(0, SeqSource::Kit, 2, "/crosspad/kits/A/kit.json"));
    SeqNote n;
    n.pad = 1;
    n.vel = 100;
    a.putSeqTake(0, 0, makeSeqPattern(1, {n}), a.samplesPerBar(44100), 1);
    uint16_t feel = 0;
    SceneLookup scenes;
    scenes.swing = [&](uint8_t scene) { return scene == 2 ? feel : (uint16_t)0; };
    const uint32_t straight = seq_freeze_key(a, 0, 44100, scenes);
    CHECK(straight == seq_freeze_key(a, 0, 44100, SceneLookup{}));   // a freeze from before swing still fits
    feel = 48 | (2 << 8);
    const uint32_t swung = seq_freeze_key(a, 0, 44100, scenes);
    CHECK(swung != straight);
    feel = 48 | (4 << 8);                                // the sixteenths swung instead
    CHECK(seq_freeze_key(a, 0, 44100, scenes) != swung);
}

TEST_CASE("AudioSong: blocks with the same notes share them again after a load", "[song][tracks]") {
    const auto d = freshProject("cp_song_parts");
    AudioSong a = songAt120();
    a.dir = d;
    REQUIRE(a.addSeqTrack(0, SeqSource::Kit, 0));
    const auto partA = makeSeqPattern(1, {note(0, 1), note(96, 2)});
    REQUIRE(a.putSeqTake(0, 0, partA, kSpb) >= 0);
    REQUIRE(a.putSeqTake(0, 1, makeSeqPattern(1, {note(0, 3)}), kSpb) >= 0);
    REQUIRE(a.putSeqTake(0, 2, makeSeqPattern(1, {note(0, 1), note(96, 2)}), kSpb) >= 0);   // A again, a copy of its own
    REQUIRE(a.save());

    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    const auto& cs = b.tracks[0].clips;
    REQUIRE(cs.size() == 3);
    CHECK(cs[0].notes == cs[2].notes);
    CHECK(cs[0].notes != cs[1].notes);
    fs::remove_all(d);
}

TEST_CASE("AudioSong: a REC take replaces the seq blocks under it", "[song][seq][notes]") {
    AudioSong s = songAt120();
    REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
    const auto old = makeSeqPattern(1, {note(0, 1)});
    s.putSeqTake(0, 0, makeSeqPattern(4, {note(0, 1), note(3 * kTpb, 2)}), kSpb);   // bars 0..3
    s.putSeqTake(0, 6, old, kSpb);                                                  // bar 6
    /* Bars 2..6: cuts the first block at 2, takes bar 6 out whole. */
    const int i = s.putSeqTake(0, 2, makeSeqPattern(5, {note(0, 9)}), kSpb);
    REQUIRE(i >= 0);
    const auto& cs = s.tracks[0].clips;
    REQUIRE(cs.size() == 2);
    CHECK(cs[0].bar == 0);
    CHECK(cs[0].bars == 2);
    CHECK(cs[1].bar == 2);
    CHECK(cs[1].bars == 5);
    REQUIRE(cs[1].notes);
    CHECK(cs[1].notes->notes.size() == 1);
    CHECK(cs[1].notes->notes[0].pad == 9);
}

TEST_CASE("AudioSong: a take longer than a scene is laid as blocks the seq view opens", "[song][seq][notes]") {
    AudioSong s = songAt120();
    REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
    /* 19 bars from bar 2: a hit in the first, one held from bar 8 over the
     * line into bar 9, and one in the last. */
    const int i = s.putSeqTakeNotes(0, 2, 19, {note(18 * kTpb + 5, 3), note(0, 1), note(7 * kTpb + kTpb / 2, 2, 100, kTpb)}, kSpb);
    REQUIRE(i == 0);
    const auto& cs = s.tracks[0].clips;
    REQUIRE(cs.size() == 3);
    CHECK(cs[0].bar == 2);
    CHECK(cs[0].bars == AudioSong::kSeqBlockBars);
    CHECK(cs[1].bar == 2 + AudioSong::kSeqBlockBars);
    CHECK(cs[1].bars == AudioSong::kSeqBlockBars);
    CHECK(cs[2].bar == 2 + 2 * AudioSong::kSeqBlockBars);
    CHECK(cs[2].bars == 3);
    REQUIRE(cs[0].notes->notes.size() == 2);
    CHECK(cs[0].notes->notes[0].pad == 1);
    CHECK(cs[0].notes->notes[1].len == kTpb / 2);       // ends on the line into the next block
    CHECK(cs[1].notes->notes.empty());
    REQUIRE(cs[2].notes->notes.size() == 1);
    CHECK(cs[2].notes->notes[0].tick == 2 * kTpb + 5);
    CHECK(cs[2].notes->notes[0].pad == 3);
}

TEST_CASE("AudioSong: a block of a take holds no more notes than a scene", "[song][seq][notes]") {
    AudioSong s = songAt120();
    REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
    /* 64 hits a bar over the first four bars of eight: 256 in eight bars or
     * in four is more than a scene holds, 128 in two is not. */
    std::vector<SeqNote> dense;
    for (uint32_t t = 0; t < 4 * 64; ++t) dense.push_back(note(t * (kTpb / 64), (uint8_t)(t % 16)));
    s.putSeqTakeNotes(0, 0, 8, dense, kSpb);
    const auto& cs = s.tracks[0].clips;
    REQUIRE(cs.size() == 2);
    CHECK(cs[0].bars == 2);
    CHECK(cs[1].bar == 2);
    CHECK(cs[1].bars == 6);                   // the rest, which fits
    for (const auto& c : cs) CHECK(c.notes->notes.size() <= AudioSong::kSeqBlockNotes);
    CHECK(cs[0].notes->notes.size() + cs[1].notes->notes.size() == dense.size());
}

TEST_CASE("AudioSong: DUB under no block lays blocks the seq view opens", "[song][seq][notes]") {
    AudioSong s = songAt120();
    REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
    REQUIRE(s.dubSeqNotes(0, {note(kTpb, 1), note(12 * kTpb, 2)}));
    const auto& cs = s.tracks[0].clips;
    REQUIRE(cs.size() == 2);
    CHECK(cs[0].bar == 1);
    CHECK(cs[0].bars == AudioSong::kSeqBlockBars);
    CHECK(cs[1].bar == 1 + AudioSong::kSeqBlockBars);
    CHECK(cs[1].bars == 4);
    REQUIRE(cs[1].notes->notes.size() == 1);
    CHECK(cs[1].notes->notes[0].tick == 3 * kTpb);
}

TEST_CASE("AudioSong: a take over the middle of a block leaves its tail playing on", "[song][seq][notes]") {
    AudioSong s = songAt120();
    REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
    const auto long8 = makeSeqPattern(8, {note(0, 1), note(5 * kTpb, 2)});
    s.putSeqTake(0, 0, long8, kSpb);
    s.putSeqTake(0, 2, makeSeqPattern(2, {note(0, 7)}), kSpb);
    const auto& cs = s.tracks[0].clips;
    REQUIRE(cs.size() == 3);
    CHECK(cs[2].bar == 4);
    CHECK(cs[2].from == 4);                   // the old notes from their bar four
    CHECK(cs[2].notes == long8);              // shared, not copied
}

TEST_CASE("AudioSong: DUB adds into the block it falls in, and a copy keeps its own", "[song][seq][notes]") {
    AudioSong s = songAt120();
    REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
    const auto first = makeSeqPattern(2, {note(0, 1)});
    s.putSeqTake(0, 0, first, kSpb);
    SongClip copy = s.tracks[0].clips[0];     // DUP: the same notes, elsewhere
    copy.bar = 8;
    s.addClip(0, copy);
    /* Bar 1 beat 2 of the song: inside the first block. Bar 4: nothing there. */
    REQUIRE(s.dubSeqNotes(0, {note(kTpb + 96, 4), note(4 * kTpb + 10, 5), note(5 * kTpb, 6)}));
    const auto& cs = s.tracks[0].clips;
    REQUIRE(cs.size() == 3);
    REQUIRE(cs[0].notes != first);
    REQUIRE(cs[0].notes->notes.size() == 2);
    CHECK(cs[0].notes->notes[1].tick == kTpb + 96);
    /* What fell under nothing is a block of its own over its bars. */
    CHECK(cs[1].bar == 4);
    CHECK(cs[1].bars == 2);
    REQUIRE(cs[1].notes->notes.size() == 2);
    CHECK(cs[1].notes->notes[0].tick == 10);
    CHECK(cs[1].notes->notes[1].tick == kTpb);
    /* The copy was not edited: it keeps the notes it had. */
    CHECK(cs[2].bar == 8);
    CHECK(cs[2].notes == first);
}

TEST_CASE("AudioSong: DUB into a block played from its middle lands on the right bar of it", "[song][seq][notes]") {
    AudioSong s = songAt120();
    REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
    s.putSeqTake(0, 0, makeSeqPattern(4, {}), kSpb);
    s.cutClip(0, 2, kSpb);                    // the second half plays bars 2..3 of the notes
    REQUIRE(s.dubSeqNotes(0, {note(3 * kTpb, 8)}));
    const auto& cs = s.tracks[0].clips;
    REQUIRE(cs.size() == 2);
    CHECK(cs[0].notes->notes.empty());        // the first half kept its own
    REQUIRE(cs[1].notes->notes.size() == 1);
    CHECK(cs[1].notes->notes[0].tick == 3 * kTpb);
}

TEST_CASE("AudioSong: blocks that played the scene adopt its notes", "[song][seq][notes]") {
    AudioSong s = songAt120();
    REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 0));
    s.tracks[1].seqBlocks = false;   // from before blocks had notes
    /* No blocks yet: the scene over the whole song becomes one block of it. */
    const auto scene = makeSeqPattern(2, {note(0, 3)});
    CHECK(s.adoptSceneNotes(1, scene, 6) == 1);
    REQUIRE(s.tracks[1].seqBlocks);
    REQUIRE(s.tracks[1].clips.size() == 1);
    CHECK(s.tracks[1].clips[0].bars == 6);
    CHECK(s.tracks[1].clips[0].notes == scene);
    CHECK(s.adoptSceneNotes(1, scene, 6) == 0);   // nothing left to adopt

    /* An empty scene leaves no block of silence behind. */
    AudioSong e = songAt120();
    REQUIRE(e.addSeqTrack(0, SeqSource::Kit, 0));
    e.tracks[0].seqBlocks = false;   // from before blocks had notes
    e.seqBlocksExplicit(4);
    CHECK(e.adoptSceneNotes(0, makeSeqPattern(1, {}), 4) == 1);
    CHECK(e.tracks[0].clips.empty());
}

TEST_CASE("AudioSong: a region copy of a seq block shares its notes", "[song][seq][notes]") {
    AudioSong s = songAt120();
    REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
    const auto p = makeSeqPattern(4, {note(0, 1)});
    s.putSeqTake(0, 0, p, kSpb);
    const auto got = s.copyRange(1, 3, kSpb);
    REQUIRE(got.size() == 1);
    CHECK(got[0].clip.notes == p);
    CHECK(got[0].clip.from == 1);
    s.insertBars(4, 2, kSpb);
    s.pasteRange(got, 4);
    const auto& cs = s.tracks[0].clips;
    REQUIRE(cs.size() == 2);
    CHECK(cs[1].bar == 4);
    CHECK(cs[1].notes == p);
}

TEST_CASE("AudioSong: clearing a range cuts what reaches in, trims what reaches past and drops the rest", "[song][region]") {
    AudioSong s = songAt120();
    s.tracks[0].inUse = true;
    s.addClip(0, {"a.wav", 0, 4});         // bars 0-3: cut at both ends
    s.addClip(0, {"b.wav", 5, 1});         // bar 5: out of it
    s.tracks[1].inUse = true;
    s.addClip(1, {"c.wav", 1, 1});         // bar 1: inside, goes
    s.clearRange(1, 3, kSpb);
    const auto& a = s.tracks[0].clips;
    REQUIRE(a.size() == 3);
    CHECK((a[0].bar == 0 && a[0].bars == 1));
    CHECK((a[1].bar == 3 && a[1].bars == 1));
    CHECK(a[1].offset == 3 * kSpb);        // the rest plays on from where it was
    CHECK(a[2].bar == 5);
    CHECK(s.tracks[1].clips.empty());
}

TEST_CASE("AudioSong: a clip name that leaves clips/ loads as a block with no file", "[song][load]") {
    const auto d = freshProject("cp_song_names");
    AudioSong a = songAt120();
    a.dir = d;
    a.tracks[0].inUse = true;
    a.addClip(0, {"good.wav", 0, 1});
    a.addClip(0, {"evil.wav", 1, 1});
    a.addClip(0, {"back.wav", 2, 1});
    REQUIRE(a.addSeqTrack(1, SeqSource::Kit, 0));
    a.tracks[1].freeze.push_back({".freeze/FRZ00000001.wav", 0, 1});
    a.tracks[1].freeze.push_back({"../../evil.wav", 1, 1});
    REQUIRE(a.save());
    std::string text;
    {
        std::ifstream in(d + "/song.json");
        text.assign(std::istreambuf_iterator<char>(in), {});
    }
    for (const auto& [from, to] : {std::pair<const char*, const char*>{"evil.wav", "../evil.wav"}, {"back.wav", "x\\\\y.wav"}}) {
        const size_t at = text.find(from);
        REQUIRE(at != std::string::npos);
        text.replace(at, std::strlen(from), to);
    }
    std::ofstream(d + "/song.json") << text;
    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    REQUIRE(b.tracks[0].clips.size() == 1);          // the dropped ones are gone, as a block with no file is
    CHECK(b.tracks[0].clips[0].file == "good.wav");
    REQUIRE(b.tracks[1].freeze.size() == 1);
    CHECK(b.tracks[1].freeze[0].file == ".freeze/FRZ00000001.wav");
    CHECK(is_clip_file_name("a b.wav"));
    for (const char* bad : {"", ".", "..", "a/b.wav", "a\\b.wav", "/abs.wav"}) CHECK_FALSE(is_clip_file_name(bad));
}

TEST_CASE("AudioSong: a bounced track's audio in the freeze folder survives a save", "[song][load]") {
    const auto d = freshProject("cp_song_bounced");
    AudioSong a = songAt120();
    a.dir = d;
    a.tracks[0].inUse = true;
    a.addClip(0, {".freeze/FRZ0000000A.wav", 0, 2});    // what BOUNCE leaves on the track
    a.addClip(0, {".freeze/FRZ0000000A.wav", 2, 2});    // and its DUP
    a.addClip(0, {"take.wav", 4, 1});
    REQUIRE(a.save());
    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    REQUIRE(b.tracks[0].clips.size() == 3);
    CHECK(b.tracks[0].clips[0].file == ".freeze/FRZ0000000A.wav");
    CHECK(b.tracks[0].clips[1].file == ".freeze/FRZ0000000A.wav");
    CHECK(is_song_clip_name("take.wav"));
    CHECK(is_song_clip_name(".freeze/FRZ0000000A.wav"));
    for (const char* bad : {".freeze/", ".freeze/../x.wav", ".freeze/a/b.wav", "other/x.wav", "../x.wav"})
        CHECK_FALSE(is_song_clip_name(bad));
}

TEST_CASE("AudioSong: the song's kit and instrument survive a save, and a Pitch track is found", "[song][sounds]") {
    const auto d = freshProject("cp_song_sounds");
    AudioSong a = songAt120();
    a.dir = d;
    a.kit = "/crosspad/kits/DIPPER_-_DIPPER_kit/kit.json";
    a.instrument = "pitch/instrument.json";
    CHECK_FALSE(a.hasPitchTrack());
    REQUIRE(a.addSeqTrack(1, SeqSource::Pitch, 0));
    CHECK(a.hasPitchTrack());
    REQUIRE(a.save());
    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    CHECK(b.kit == a.kit);
    CHECK(b.instrument == a.instrument);
    CHECK(b.hasPitchTrack());
    /* A song without them has none, and loading it clears what was there. */
    AudioSong c = songAt120();
    c.dir = d;
    REQUIRE(c.save());
    REQUIRE(b.load(d) == SongLoad::Loaded);
    CHECK(b.kit.empty());
    CHECK(b.instrument.empty());
    fs::remove_all(d);
}

TEST_CASE("AudioSong: an audio track keeps its input and can be armed", "[song][tracks][input]") {
    const auto d = freshProject("cp_song_input");
    AudioSong a;
    a.dir = d;
    REQUIRE(a.addAudioTrack(0));
    REQUIRE(a.addAudioTrack(1));
    REQUIRE(a.addSeqTrack(2, SeqSource::Kit, 0));
    CHECK(a.tracks[0].input == RecInput::Mic);              // the default
    a.tracks[1].input = RecInput::MicLine;
    a.armed = 1;
    CHECK(a.armedAudioTrack() == 1);
    CHECK(a.armedSeqTrack() == 2);                          // the seq takes still find their track
    REQUIRE(a.save());

    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    CHECK(b.tracks[0].input == RecInput::Mic);
    CHECK(b.tracks[1].input == RecInput::MicLine);
    CHECK(b.armedAudioTrack() == 1);
    b.armed = 2;
    CHECK(b.armedAudioTrack() == -1);                       // a seq track is no audio track
    b.armed = 3;
    CHECK(b.armedAudioTrack() == -1);                       // nor is one nobody made
    fs::remove_all(d);
}

TEST_CASE("AudioSong: a track's colour survives song.json, moves with the track, and is out of range read as its kind's",
          "[song][tracks][colour]") {
    const auto d = freshProject("cp_song_colour");
    AudioSong a;
    a.dir = d;
    REQUIRE(a.addAudioTrack(0));
    REQUIRE(a.addSeqTrack(1, SeqSource::Kit, 0));
    CHECK(a.tracks[0].colour == 0);
    a.tracks[1].colour = 5;
    REQUIRE(a.save());
    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    CHECK(b.tracks[0].colour == 0);
    CHECK(b.tracks[1].colour == 5);
    REQUIRE(b.swapTracks(0, 1));
    CHECK(b.tracks[0].colour == 5);
    b.removeTrack(0);
    CHECK(b.tracks[0].colour == 0);
    b.tracks[0].colour = 200;
    REQUIRE(b.save());
    AudioSong c;
    REQUIRE(c.load(d) == SongLoad::Loaded);
    CHECK(c.tracks[0].colour == 0);
    fs::remove_all(d);
}

namespace {
std::string fileText(const std::string& path)
{
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE("audio song: an element clip and a title survive song.json, version 5", "[song][element]") {
    const auto d = freshProject("cp_song_element");
    AudioSong a;
    a.dir = d;
    a.bpm = 120.0f;
    a.title = "REFREN";
    SongClip c;
    c.element = ".elements/E00C0FFEE";
    c.file = "B0123456789ABCDEF.wav";
    c.bar = 4;
    c.bars = 8;
    c.from = 1;
    c.frames = 176400;
    a.addClip(1, c);
    REQUIRE(a.save());
    CHECK(fileText(d + "/song.json").find("\"version\":5") != std::string::npos);

    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    CHECK(b.title == "REFREN");
    REQUIRE(b.tracks[1].clips.size() == 1);
    const SongClip& r = b.tracks[1].clips[0];
    CHECK(r.isElement());
    CHECK(r.element == c.element);
    CHECK(r.file == c.file);
    CHECK(r.bar == 4);
    CHECK(r.bars == 8);
    CHECK(r.from == 1);
    CHECK(r.frames == 176400);
}

TEST_CASE("audio song: an element clip not baked yet is kept", "[song][element]") {
    const auto d = freshProject("cp_song_element_unbaked");
    AudioSong a;
    a.dir = d;
    SongClip c;
    c.element = ".elements/E00000001";
    c.bar = 0;
    c.bars = 2;
    a.addClip(0, c);
    REQUIRE(a.save());
    AudioSong b;
    REQUIRE(b.load(d) == SongLoad::Loaded);
    REQUIRE(b.tracks[0].clips.size() == 1);
    CHECK(b.tracks[0].clips[0].element == c.element);
    CHECK(b.tracks[0].clips[0].file.empty());
}

TEST_CASE("audio song: a song without elements keeps its old version", "[song][element]") {
    const auto d = freshProject("cp_song_no_element");
    AudioSong a;
    a.dir = d;
    a.bpm = 120.0f;
    REQUIRE(a.addSeqTrack(0, SeqSource::Kit, 0, "/crosspad/kits/DRUMS/kit.json"));
    REQUIRE(a.addSeqTrack(2, SeqSource::Pitch, 2));
    REQUIRE(a.save());
    const std::string text = fileText(d + "/song.json");
    CHECK(text.find("\"version\":4") != std::string::npos);
    CHECK(text.find("\"element\"") == std::string::npos);
    CHECK(text.find("\"title\"") == std::string::npos);
}

TEST_CASE("audio song: element ids and refs", "[song][element]") {
    CHECK(make_element_id(0xC0FFEEu) == "E00C0FFEE");
    CHECK(is_element_ref(".elements/E00C0FFEE"));
    CHECK_FALSE(is_element_ref(".elements/e00c0ffee"));
    CHECK_FALSE(is_element_ref(".elements/E00C0FFE"));
    CHECK_FALSE(is_element_ref("../x"));
    CHECK_FALSE(is_element_ref("Song 1"));
}

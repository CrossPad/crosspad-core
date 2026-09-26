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

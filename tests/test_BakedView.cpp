// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "crosspad/song/BakedView.hpp"

#include <set>

using namespace crosspad;

namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kBar  = 2 * kRate;   // a bar at 120 BPM, 4 beats
const char* const kElem  = ".elements/E00000001";

struct View {
    AudioSong song;
    std::set<std::string> files;
    ResolvedElement child;
    BakedViewEnv env;

    View() {
        song.dir = "/songs/s";
        song.bpm = 120.0f;
        child.ref = kElem;
        child.file = "B0000000000000001.wav";
        child.frames = 2 * kBar;
        child.exists = true;
        env.songsRoot = "/songs";
        env.sampleRate = kRate;
        env.bpm = 120.0f;
        env.exists = [this](const std::string& p) { return files.count(p) != 0; };
        env.child = [this](const std::string& ref) { return ref == child.ref ? &child : nullptr; };
    }

    SongTrack& track(int t, TrackKind kind) {
        song.tracks[t].inUse = true;
        song.tracks[t].kind = kind;
        return song.tracks[t];
    }
    std::vector<BakedPiece> view(uint32_t a, uint32_t b, BakedReport& r) { return baked_view(song, env, a, b, r); }
};

SongClip block(const std::string& file, uint32_t bar, uint16_t bars) {
    SongClip c;
    c.file = file;
    c.bar = bar;
    c.bars = bars;
    return c;
}

} // namespace

TEST_CASE("baked view: recordings, a frozen seq track and an element going round their bake", "[song][baked]") {
    View v;
    SongTrack& audio = v.track(0, TrackKind::Audio);
    audio.volume = 0.5f;
    audio.pan = -0.5f;
    v.song.addClip(0, block("a.wav", 0, 2));
    SongTrack& seq = v.track(1, TrackKind::Seq);
    seq.pan = 0.25f;
    seq.freeze.push_back(block("f.wav", 0, 2));
    seq.freeze.back().offset = 100;
    SongClip e = block("", 2, 3);
    e.element = kElem;
    e.from = 1;
    v.track(2, TrackKind::Audio);
    v.song.addClip(2, e);
    v.files = {"/songs/s/clips/a.wav", "/songs/s/clips/f.wav"};

    BakedReport r;
    const auto p = v.view(0, 5 * kBar, r);
    CHECK(r.complete());
    REQUIRE(p.size() == 4);
    CHECK(p[0].path == "/songs/s/clips/a.wav");
    CHECK(p[0].start == 0);
    CHECK(p[0].end == 2 * kBar);
    CHECK(p[0].gain == 0.5f);
    CHECK(p[0].pan == -0.5f);
    CHECK(p[1].path == "/songs/s/clips/f.wav");
    CHECK(p[1].offset == 100);
    CHECK(p[1].gain == 1.0f);
    CHECK(p[1].pan == 0.25f);
    /* Starting on its bar 1 of 2: the rest of one pass, then a whole one cut by the block. */
    CHECK(p[2].path == "/songs/.bake/B0000000000000001.wav");
    CHECK(p[2].start == 2 * kBar);
    CHECK(p[2].end == 3 * kBar);
    CHECK(p[2].offset == kBar);
    CHECK(p[2].pan == 0.0f);
    CHECK(p[3].start == 3 * kBar);
    CHECK(p[3].end == 5 * kBar);
    CHECK(p[3].offset == 0);
}

TEST_CASE("baked view: a window cuts the pieces and moves their offsets", "[song][baked]") {
    View v;
    v.track(0, TrackKind::Audio);
    v.song.addClip(0, block("a.wav", 0, 4));
    v.files = {"/songs/s/clips/a.wav"};
    BakedReport r;
    const auto p = v.view(kBar, 3 * kBar, r);
    REQUIRE(p.size() == 1);
    CHECK(p[0].start == kBar);
    CHECK(p[0].end == 3 * kBar);
    CHECK(p[0].offset == kBar);
    CHECK(v.view(5 * kBar, 6 * kBar, r).empty());
}

TEST_CASE("baked view: a stretched track plays its fitted copy when there is one", "[song][baked]") {
    View v;
    SongTrack& t = v.track(0, TrackKind::Audio);
    t.stretch = StretchMode::KeepPitch;
    SongClip c = block("a.wav", 0, 2);
    c.offset = 10;
    c.frames = kBar;
    v.song.addClip(0, c);
    const std::string fitted = v.song.clipPath(stretchName(c, 2 * kBar, StretchMode::KeepPitch));
    v.files = {"/songs/s/clips/a.wav"};
    BakedReport r;
    auto p = v.view(0, 2 * kBar, r);
    REQUIRE(p.size() == 1);
    CHECK(p[0].path == "/songs/s/clips/a.wav");
    CHECK(p[0].end == kBar);
    CHECK(p[0].offset == 10);
    CHECK_FALSE(r.complete());   // laid as recorded, but not fitted yet
    CHECK(r.missingFiles == std::vector<std::string>{fitted});
    v.files.insert(fitted);
    r = BakedReport{};
    p = v.view(0, 2 * kBar, r);
    CHECK(r.complete());
    REQUIRE(p.size() == 1);
    CHECK(p[0].path == fitted);
    CHECK(p[0].end == 2 * kBar);
    CHECK(p[0].offset == 0);
}

TEST_CASE("baked view: what cannot be heard from files is reported", "[song][baked]") {
    View v;
    v.track(0, TrackKind::Seq);                        // never frozen
    v.track(1, TrackKind::Seq).freeze.push_back(block("gone.wav", 0, 1));   // freeze not on the card
    v.track(2, TrackKind::Audio);
    v.song.addClip(2, block("lost.wav", 0, 1));
    SongClip e = block("", 0, 1);
    e.element = ".elements/E000000FF";
    v.song.addClip(2, e);
    BakedReport r;
    CHECK(v.view(0, kBar, r).empty());
    CHECK(r.unfrozenSeq == std::vector<int>{0});
    CHECK(r.missingFiles.size() == 2);
    CHECK(r.unresolved == std::vector<std::string>{".elements/E000000FF"});
    CHECK_FALSE(r.complete());
}

TEST_CASE("baked view: a silenced track and the silent element give nothing", "[song][baked]") {
    View v;
    v.track(0, TrackKind::Audio).muted = true;
    v.song.addClip(0, block("a.wav", 0, 1));
    SongClip e = block("", 0, 2);
    e.element = kElem;
    v.track(1, TrackKind::Audio);
    v.song.addClip(1, e);
    v.files = {"/songs/s/clips/a.wav"};
    BakedReport r;
    CHECK(v.view(0, 2 * kBar, r).size() == 1);   // only the element's pass
    v.env.silentRefs = {kElem};
    CHECK(v.view(0, 2 * kBar, r).empty());
    CHECK(r.complete());
}

TEST_CASE("baked view: a block already at its length needs no fitted copy", "[song][baked]") {
    View v;
    v.track(0, TrackKind::Audio).stretch = StretchMode::KeepPitch;
    SongClip c = block("a.wav", 0, 2);
    c.frames = 2 * kBar;
    v.song.addClip(0, c);
    v.files = {"/songs/s/clips/a.wav"};
    BakedReport r;
    CHECK(v.view(0, 2 * kBar, r).size() == 1);
    CHECK(r.complete());
}

TEST_CASE("baked view: a freeze rendered at another tempo is reported, not laid", "[song][baked]") {
    View v;
    v.track(0, TrackKind::Seq).freeze.push_back(block("f.wav", 0, 2));
    v.files = {"/songs/s/clips/f.wav"};
    v.env.bpm = 90.0f;
    BakedReport r;
    CHECK(v.view(0, 4 * kBar, r).empty());
    CHECK(r.unfrozenSeq == std::vector<int>{0});
}

TEST_CASE("baked view: corrupt bars and lengths end, with bounded pieces", "[song][baked]") {
    View v;
    v.track(0, TrackKind::Audio);
    v.song.addClip(0, block("a.wav", 0xFFFFFF00u, 0xFFFF));
    SongClip e = block("", 0, 0xFFFF);
    e.element = kElem;
    e.from = 0xFFFF;
    v.song.addClip(0, e);
    v.child.frames = 0xFFFFFFF0u;
    v.files = {"/songs/s/clips/a.wav"};
    BakedReport r;
    CHECK(v.view(0, UINT32_MAX, r).size() < 8);
    uint32_t n = 0;
    element_passes(1, 0, kBar, 0, UINT32_MAX, [&](uint32_t, uint32_t, uint32_t) { ++n; });
    CHECK(n == 0);
    element_passes(0, 0xFFFFFFF0u, kBar, 0xFFFFFF00u, UINT32_MAX, [&](uint32_t, uint32_t, uint32_t) { ++n; });
    CHECK(n == 1);
}

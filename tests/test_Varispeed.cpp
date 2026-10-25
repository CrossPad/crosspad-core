// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stale bakes played varispeed: after a tempo change an element's bake made at
// the old tempo goes on playing, resampled to a pass of the element's bars at
// the new one (a little higher or lower), until the bake at the new tempo is
// made -- in playback, in the engine's own bake and in the context stem alike.

#include "song_engine_rig.hpp"

#include "crosspad/song/BakedView.hpp"
#include "crosspad/song/ElementNav.hpp"

#include <cmath>

namespace {

constexpr float kBakeBpm = 120.0f;
constexpr float kSongBpm = 100.0f;
constexpr uint32_t kBakeBar = 4 * 22050;    // a bar at 120 BPM, 44.1 kHz
constexpr uint32_t kSongBar = 4 * 26460;    // a bar at 100 BPM
constexpr double kToneHz = 1000.0;
constexpr int16_t kToneLevel = 16000;
constexpr int kWaitMs = 10000;
const char* const kE = ".elements/E00000001";
const char* const kF = ".elements/E00000002";
const char* const kG = ".elements/E00000003";
const std::string kBake = "B0000000000000001.wav";

/// Varispeed is linear interpolation: what a pass sounds at, within this.
constexpr double kPitchTolerance = 0.01;

struct NavGuard {
    NavGuard() { elementNav().reset(); }
    ~NavGuard() { elementNav().reset(); }
};

fs::path bakeDir(Rig& r) { return fs::path(r.platform.root) / AudioSong::bakeDir(); }

void writeTone(const std::string& path, uint32_t frames) {
    WavWriter w;
    REQUIRE(w.open(path.c_str(), kRate, 2));
    std::vector<int16_t> buf(frames * 2);
    for (uint32_t i = 0; i < frames; ++i)
        buf[i * 2] = buf[i * 2 + 1] = (int16_t)std::lround(kToneLevel * std::sin(2.0 * M_PI * kToneHz * i / kRate));
    REQUIRE(w.write(buf.data(), frames));
    REQUIRE(w.close());
}

/// A bake whose every frame says where it is: frame i is i % 30000 + 1.
void writeRamp(const std::string& path, uint32_t frames) {
    WavWriter w;
    REQUIRE(w.open(path.c_str(), kRate, 2));
    std::vector<int16_t> buf(frames * 2);
    for (uint32_t i = 0; i < frames; ++i) buf[i * 2] = buf[i * 2 + 1] = (int16_t)(i % 30000 + 1);
    REQUIRE(w.write(buf.data(), frames));
    REQUIRE(w.close());
}

/// The pitch of a tone from its zero crossings, @p n frames of the left channel from @p at.
double toneHz(const std::vector<int16_t>& left, size_t at, size_t n) {
    REQUIRE(left.size() >= at + n);
    int crossings = 0;
    for (size_t i = at + 1; i < at + n; ++i)
        if ((left[i - 1] < 0) != (left[i] < 0)) ++crossings;
    return crossings / 2.0 / ((double)n / kRate);
}

/// "Demo" open at @p songBpm with a block of @p ref on track 0 over @p bars bars,
/// holding @p bake of @p frames made at @p bakeBpm. No element of that ref is
/// on the card: nothing relinks the block.
void openWithBlock(Rig& r, float songBpm, uint16_t bars, uint32_t frames, float bakeBpm) {
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.bpm = songBpm;
        s.tracks[0].inUse = true;
        SongClip c;
        c.element = kE;
        c.file = kBake;
        c.bars = bars;
        c.frames = frames;
        c.bakeBpm = bakeBpm;
        s.addClip(0, c);
    }
    r.engine->commit();
}

/// Plays from bar 0 and gives the left channel of what is heard from its first
/// sound on, @p frames of it, with no underrun on the way.
std::vector<int16_t> heard(Rig& r, uint32_t frames) {
    REQUIRE(r.engine->play(0));
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    std::vector<int16_t> left;
    REQUIRE(waitFor([&] {
        r.block(out, bus);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        for (uint32_t i = 0; i < kBlock; ++i) {
            const int16_t v = (int16_t)std::lround(out[i * 2] * 32768.0f);
            if (left.empty() && v == 0) continue;
            left.push_back(v);
        }
        return left.size() >= frames;
    }, 20000));
    CHECK(r.engine->underruns() == 0);
    left.resize(frames);
    return left;
}

SongClip clipOf(Rig& r, int track) {
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    return r.engine->song().tracks[track].clips.at(0);
}

bool idle(Rig& r) {
    return waitFor([&] { return !r.engine->backgroundBusy(); }, kWaitMs);
}

/// An audio element on the card at @p bpm: one track of @p bars bars of @p value.
void writeAudioElement(Rig& r, const std::string& ref, float bpm, uint16_t bars, int16_t value) {
    AudioSong s;
    s.dir = r.platform.root + "/" + ref;
    fs::create_directories(fs::path(s.dir) / AudioSong::clipsDir());
    s.bpm = bpm;
    writeClip(s.clipPath("own.wav"), bars * AudioSong::samplesPerBar(kRate, bpm, 4), value);
    s.tracks[0].inUse = true;
    s.addClip(0, {"own.wav", 0, bars});
    REQUIRE(s.save());
}

} // namespace

TEST_CASE("varispeed: a bake's pass at another tempo is its beats at the song's", "[song][element][varispeed]") {
    const uint32_t beat = kSongBar / 4;
    CHECK(varispeed_frames(2 * kBakeBar, kBakeBpm, kSongBpm, beat) == 2 * kSongBar);
    CHECK(varispeed_frames(2 * kSongBar, kSongBpm, kBakeBpm, kBakeBar / 4) == 2 * kBakeBar);
    /* The same tempo, or none kept (a block linked before bakes kept it): as it is. */
    CHECK(varispeed_frames(2 * kBakeBar, kBakeBpm, kBakeBpm, kBakeBar / 4) == 2 * kBakeBar);
    CHECK(varispeed_frames(2 * kBakeBar, 0.0f, kSongBpm, beat) == 2 * kBakeBar);
    CHECK(varispeed_step(2 * kBakeBar, 2 * kBakeBar) == 0);
    CHECK(varispeed_step(2 * kBakeBar, 2 * kSongBar) == ((uint64_t)2 * kBakeBar << kVarispeedFracBits) / (2 * kSongBar));
}

TEST_CASE("varispeed: the baked view lays a bake made at another tempo as playback does", "[song][element][varispeed][baked]") {
    AudioSong song;
    song.dir = "/songs/s";
    song.bpm = kSongBpm;
    song.tracks[0].inUse = true;
    SongClip c;
    c.element = kE;
    c.bars = 2;
    song.addClip(0, c);
    ResolvedElement child;
    child.ref = kE;
    child.file = kBake;
    child.frames = kBakeBar;
    child.exists = true;
    BakedViewEnv env;
    env.songsRoot = "/songs";
    env.sampleRate = kRate;
    env.bpm = kSongBpm;
    env.exists = [](const std::string&) { return true; };
    env.child = [&](const std::string&) { return &child; };
    BakedReport report;
    SECTION("made at 120: a bar at 100 a pass, read at the bake's speed") {
        child.bpm = kBakeBpm;
        const auto p = baked_view(song, env, 0, 2 * kSongBar, report);
        REQUIRE(p.size() == 2);
        CHECK(p[0].start == 0);
        CHECK(p[0].end == kSongBar);
        CHECK(p[1].start == kSongBar);
        CHECK(p[1].end == 2 * kSongBar);
        CHECK(p[1].offset == 0);
        CHECK(p[0].step == varispeed_step(kBakeBar, kSongBar));
    }
    SECTION("made at the song's tempo: as it is") {
        child.bpm = kSongBpm;
        child.frames = kSongBar;
        const auto p = baked_view(song, env, 0, 2 * kSongBar, report);
        REQUIRE(p.size() == 2);
        CHECK(p[0].step == 0);
    }
}

TEST_CASE("varispeed: a 1 kHz bake made at 120 BPM plays at 833 Hz in a 100 BPM song, a bar at 100 a pass",
          "[song][element][varispeed]") {
    Rig r;
    fs::create_directories(bakeDir(r));
    writeTone((bakeDir(r) / kBake).string(), kBakeBar);
    openWithBlock(r, kSongBpm, 2, kBakeBar, kBakeBpm);
    /* Two passes of a bar at 100 fill the two bars; a pass of the bake's own
     * length would go round three times. */
    CHECK(segmentsOf(*r.engine, 0) == 2);
    const auto left = heard(r, kSongBar);
    const double want = kToneHz * kSongBpm / kBakeBpm;
    const double got = toneHz(left, 0, kSongBar - 1);
    INFO("heard " << got << " Hz");
    CHECK(std::fabs(got - want) <= want * kPitchTolerance);
}

TEST_CASE("varispeed: a bake at the song's tempo plays sample for sample", "[song][element][varispeed]") {
    Rig r;
    fs::create_directories(bakeDir(r));
    SECTION("its tempo kept") {
        writeRamp((bakeDir(r) / kBake).string(), kBakeBar);
        openWithBlock(r, kBakeBpm, 2, kBakeBar, kBakeBpm);
        CHECK(segmentsOf(*r.engine, 0) == 2);
        const auto left = heard(r, kBakeBar);
        for (uint32_t i = 0; i < kBakeBar; ++i)
            if (left[i] != (int16_t)(i % 30000 + 1)) FAIL("frame " << i << " is " << left[i]);
    }
    SECTION("linked before bakes kept their tempo: taken as the song's, never resampled") {
        writeRamp((bakeDir(r) / kBake).string(), kBakeBar);
        openWithBlock(r, kSongBpm, 2, kBakeBar, 0.0f);
        CHECK(segmentsOf(*r.engine, 0) == 3);   // 2 bars at 100 over passes of a bar at 120
        const auto left = heard(r, kBakeBar);
        for (uint32_t i = 0; i < kBakeBar; ++i)
            if (left[i] != (int16_t)(i % 30000 + 1)) FAIL("frame " << i << " is " << left[i]);
    }
}

TEST_CASE("varispeed: an element's bake keeps its tempo in song.json", "[song][element][varispeed]") {
    const fs::path dir = fs::temp_directory_path() / ("varispeed_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    AudioSong s;
    s.dir = dir.string();
    s.bpm = kSongBpm;
    s.tracks[0].inUse = s.tracks[1].inUse = true;
    SongClip c;
    c.element = kE;
    c.file = kBake;
    c.frames = kBakeBar;
    c.bakeBpm = kBakeBpm;
    s.addClip(0, c);
    c.element = kF;
    c.bakeBpm = 0.0f;
    s.addClip(1, c);
    REQUIRE(s.save());
    AudioSong back;
    REQUIRE(back.load(dir.string()) == SongLoad::Loaded);
    CHECK(back.tracks[0].clips.at(0).bakeBpm == kBakeBpm);
    CHECK(back.tracks[1].clips.at(0).bakeBpm == 0.0f);
    std::string text;
    {
        std::FILE* f = std::fopen((dir / AudioSong::fileName()).string().c_str(), "rb");
        REQUIRE(f);
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
    }
    CHECK(text.find("bakeBpm") == text.rfind("bakeBpm"));   // once: the block that has one
    fs::remove_all(dir);
}

TEST_CASE("varispeed: a block keeps the tempo of the bake it is linked to, and a new tempo plays it varispeed",
          "[song][element][varispeed][diskbake]") {
    Rig r;
    /* An element of a recording at 100 BPM: bakeable at 100, not at 120 (laid
     * as recorded it would not fill the bars there). */
    writeAudioElement(r, kE, kSongBpm, 2, 8000);
    REQUIRE(r.open("Parent"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.bpm = kSongBpm;
        s.tracks[0].inUse = true;
        SongClip c;
        c.element = kE;
        c.bars = 7;                                     // three passes and a half
        s.addClip(0, c);
    }
    r.engine->commit();
    REQUIRE(idle(r));
    const SongClip at100 = clipOf(r, 0);
    REQUIRE_FALSE(at100.file.empty());
    CHECK(at100.bakeBpm == kSongBpm);
    CHECK(segmentsOf(*r.engine, 0) == 4);

    REQUIRE(r.engine->requestTempo(kBakeBpm));
    REQUIRE(waitFor([&] { return r.engine->editState() != SongOpState::Busy; }));
    REQUIRE(waitFor([&] { return r.engine->elementClipState(0, 0) == ElementClipState::Stale; }, kWaitMs));
    REQUIRE(idle(r));
    CHECK(clipOf(r, 0).file == at100.file);
    CHECK(clipOf(r, 0).bakeBpm == kSongBpm);
    /* Its two bars at 100 resampled to two at 120: still three passes and a
     * half. Laid as made, a pass would be 2.4 bars at 120: three. */
    CHECK(segmentsOf(*r.engine, 0) == 4);

    /* Back at 100 its own bake is there again: linked, with the tempo it was made at. */
    REQUIRE(r.engine->requestTempo(kSongBpm));
    REQUIRE(waitFor([&] { return r.engine->editState() != SongOpState::Busy; }));
    REQUIRE(waitFor([&] { return r.engine->elementClipState(0, 0) == ElementClipState::Fresh; }, kWaitMs));
    CHECK(clipOf(r, 0).file == at100.file);
    CHECK(clipOf(r, 0).bakeBpm == kSongBpm);
}

TEST_CASE("varispeed: an element's bake holding a stale child's resamples it as playback does",
          "[song][element][varispeed]") {
    Rig r;
    fs::create_directories(bakeDir(r));
    writeTone((bakeDir(r) / kBake).string(), kBakeBar);
    /* The child is on no card: its block's bake is all there is of it. */
    openWithBlock(r, kSongBpm, 2, kBakeBar, kBakeBpm);
    REQUIRE(idle(r));
    REQUIRE(r.engine->requestBakeElement());
    REQUIRE(waitFor([&] { return r.engine->editState() != SongOpState::Busy; }, kWaitMs));
    REQUIRE(r.engine->editState() == SongOpState::Ready);
    const auto pcm = readWavData((bakeDir(r) / r.engine->bakeResult().file).string());
    REQUIRE(pcm.size() == (size_t)2 * kSongBar * 2);
    std::vector<int16_t> left(pcm.size() / 2);
    for (size_t i = 0; i < left.size(); ++i) left[i] = pcm[i * 2];
    const double want = kToneHz * kSongBpm / kBakeBpm;
    const double got = toneHz(left, 0, left.size());
    INFO("baked " << got << " Hz");
    CHECK(std::fabs(got - want) <= want * kPitchTolerance);
}

TEST_CASE("varispeed: entered, a stale block's passes are the focus's, and the stem resamples a stale neighbour",
          "[song][element][varispeed][context][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    /* Both elements at 120 and the song at 100: neither can be baked at 100
     * from the card, so their blocks keep the bakes they were made with. */
    writeAudioElement(r, kE, kBakeBpm, 2, 8000);
    writeAudioElement(r, kF, kBakeBpm, 2, 8000);
    fs::create_directories(bakeDir(r));
    const std::string eBake = "B00000000000000E1.wav";
    writeClip((bakeDir(r) / eBake).string(), 2 * kBakeBar, 4000);
    writeTone((bakeDir(r) / kBake).string(), 2 * kBakeBar);
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.bpm = kSongBpm;
        SongClip c;
        c.bars = 4;
        c.frames = 2 * kBakeBar;
        c.bakeBpm = kBakeBpm;
        c.element = kE;
        c.file = eBake;
        s.tracks[1].inUse = true;
        s.addClip(1, c);
        c.element = kF;
        c.file = kBake;
        s.tracks[2].inUse = true;
        s.addClip(2, c);
    }
    r.engine->commit();
    REQUIRE(idle(r));
    REQUIRE(clipOf(r, 2).file == kBake);

    REQUIRE(elementNav().enter(1, 0));
    ElementNav::Result res;
    waitFor([&] { res = elementNav().poll(); return res.done; }, 20000);
    INFO("error: " << res.error);
    REQUIRE(res.ok);
    /* A 4-bar block at 100 of a 2-bar element: two passes, as playback lays them. */
    const auto& in = elementNav().path()[1];
    REQUIRE(in.occurrences.size() == 2);
    CHECK(in.occurrences[1].songFrame == 2 * kSongBar);

    REQUIRE(waitFor([&] { return r.engine->contextState() == ContextState::Incomplete; }, kWaitMs));
    std::string stem;
    for (const auto& e : fs::directory_iterator(bakeDir(r)))
        if (e.path().filename().string()[0] == 'C') stem = e.path().filename().string();
    REQUIRE_FALSE(stem.empty());
    const auto pcm = readWavData((bakeDir(r) / stem).string());
    REQUIRE(pcm.size() == (size_t)2 * kSongBar * 2);
    std::vector<int16_t> left(pcm.size() / 2);
    for (size_t i = 0; i < left.size(); ++i) left[i] = pcm[i * 2];
    const double want = kToneHz * kSongBpm / kBakeBpm;
    const double got = toneHz(left, 0, left.size());
    INFO("stem " << got << " Hz");
    CHECK(std::fabs(got - want) <= want * kPitchTolerance);
}

namespace {

/// The song @p name on the card at @p bpm: a 2-bar block of each of @p refs, a
/// track each, all holding @p bake made at 120 (made before bakes kept their tempo).
void writeSongOf(Rig& r, const std::string& name, float bpm, const std::vector<std::string>& refs, const std::string& bake) {
    AudioSong s;
    s.dir = r.platform.root + "/" + name;
    fs::create_directories(fs::path(s.dir) / AudioSong::clipsDir());
    s.bpm = bpm;
    for (size_t t = 0; t < refs.size(); ++t) {
        SongClip c;
        c.element = refs[t];
        c.file = bake;
        c.bars = 2;
        c.frames = 2 * kBakeBar;
        s.tracks[t].inUse = true;
        s.addClip((int)t, c);
    }
    REQUIRE(s.save());
}

/// Block 0 of track 0 of the song @p name as the card has it.
bool cardBlock(Rig& r, const std::string& name, SongClip& out) {
    AudioSong s;
    if (s.load(r.platform.root + "/" + name) != SongLoad::Loaded || s.tracks[0].clips.empty()) return false;
    out = s.tracks[0].clips[0];
    return true;
}

} // namespace

TEST_CASE("varispeed: another song relinked while the player is away takes its own tempo, not the open song's",
          "[song][element][varispeed][diskbake]") {
    Rig r;
    const std::string old = "B00000000000000AA.wav";
    fs::create_directories(bakeDir(r));
    writeClip((bakeDir(r) / old).string(), 2 * kBakeBar, 1000);
    /* E bakes at 100 only, F at 90 only, G at 120 only: each is a recording at its tempo. */
    writeAudioElement(r, kE, kSongBpm, 2, 8000);
    writeAudioElement(r, kF, 90.0f, 2, 6000);
    writeAudioElement(r, kG, kBakeBpm, 2, 4000);
    writeSongOf(r, "A", kSongBpm, {kE, kG}, old);
    writeSongOf(r, "B", 90.0f, {kF, kG}, old);
    writeSongOf(r, "Parent", kBakeBpm, {kG}, old);
    /* A and B opened once each: E's bake at 100 and F's at 90 are on the card. */
    REQUIRE(r.open("A"));
    REQUIRE(idle(r));
    REQUIRE(r.open("B"));
    REQUIRE(idle(r));
    /* Parent open at 120 (B saved as it closes): G changed there, so A and B --
     * put back on the old bakes -- are relinked idle, E and F to the bakes
     * already made, with nothing to bake. */
    REQUIRE(r.open("Parent"));
    REQUIRE(idle(r));
    writeSongOf(r, "A", kSongBpm, {kE, kG}, old);
    writeSongOf(r, "B", 90.0f, {kF, kG}, old);
    r.engine->idleBaking(true);
    SongClip a, b;
    REQUIRE(waitFor([&] {
        return cardBlock(r, "A", a) && a.file != old && cardBlock(r, "B", b) && b.file != old;
    }, kWaitMs));
    CHECK(a.bakeBpm == kSongBpm);
    CHECK(b.bakeBpm == 90.0f);
}

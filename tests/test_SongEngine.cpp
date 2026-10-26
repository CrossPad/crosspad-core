// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <fstream>

#include "song_engine_rig.hpp"

#include "fake_file_system.hpp"
#include "crosspad/audio/WavWriter.hpp"
#include "crosspad/sequencer/MetronomeNode.hpp"
#include "crosspad/sequencer/PatternSequencer.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/kit/KitSaved.hpp"
#include "crosspad/sampletools/ISampleTools.hpp"
#include "crosspad/song/SongSounds.hpp"
#include "crosspad/song/ArrangeAudioTake.hpp"
#include "crosspad/song/SongEngine.hpp"
#include "crosspad/transport/SongTransport.hpp"
#include "crosspad/sequencer/SequenceStore.hpp"

TEST_CASE("song engine: a bad project name is refused", "[song]") {
    Rig r;
    CHECK_FALSE(r.engine->requestOpen(""));
    CHECK_FALSE(r.engine->requestOpen("a/b"));
    CHECK_FALSE(r.engine->requestOpen(".."));
    CHECK_FALSE(r.engine->requestOpen(std::string(33, 'x')));
    CHECK_FALSE(fs::exists(r.platform.root));
}

TEST_CASE("song engine: open makes the project and remembers it", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    CHECK(fs::is_directory(r.clipsDir("Demo")));
    CHECK(r.platform.current == "Demo");
    REQUIRE(r.engine->requestProjects());
    REQUIRE(waitFor([&] { return r.engine->projects() == std::vector<std::string>{"Demo"}; }));
}

TEST_CASE("song engine: the last project opens on first look", "[song]") {
    Rig r;
    r.platform.current = "Old";
    (void)r.engine->song();
    REQUIRE(waitFor([&] { return r.engine->openState() == SongOpState::Ready; }));
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    CHECK(r.engine->song().name == "Old");
}

TEST_CASE("song engine: clip list skips what is not a 16-bit WAV", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    writeClip((r.clipsDir("Demo") / "good.wav").string(), kRate * 2, 1000);
    {
        std::FILE* f = std::fopen((r.clipsDir("Demo") / "junk.wav").string().c_str(), "wb");
        std::fputs("nope", f);
        std::fclose(f);
    }
    REQUIRE(r.engine->requestClips());
    REQUIRE(waitFor([&] { return !r.engine->clips().empty(); }));
    const auto c = r.engine->clips();
    REQUIRE(c.size() == 1);
    CHECK(c[0].file == "good.wav");
}

TEST_CASE("song engine: play mixes the clip in, stop and play again is safe", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    writeClip((r.clipsDir("Demo") / "c.wav").string(), kRate * 4, 16384);   // half scale
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.bpm = 120.0f;
        SongClip c;
        c.file = "c.wav";
        c.bar = 0;
        c.bars = 2;
        s.addClip(0, c);
    }
    r.engine->commit();
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    for (int round = 0; round < 2; ++round) {
        REQUIRE(r.engine->play(0));
        REQUIRE(waitFor([&] { return r.engine->playing(); }));
        float peak = 0.0f;
        for (int i = 0; i < 2000 && peak == 0.0f; ++i) {
            r.block(out, bus);
            for (float v : out) peak = std::max(peak, v);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(peak > 0.45f);
        CHECK(peak < 0.55f);
        r.engine->stop();
        REQUIRE(waitFor([&] { return !r.engine->playing(); }));
    }
}

TEST_CASE("song engine: the meters read what plays, and solo silences the others", "[song][meter]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    writeClip((r.clipsDir("Demo") / "a.wav").string(), kRate * 8, 16384);   // half scale
    writeClip((r.clipsDir("Demo") / "b.wav").string(), kRate * 8, 8192);    // a quarter
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.bpm = 120.0f;
        s.addClip(0, {"a.wav", 0, 4});
        s.addClip(1, {"b.wav", 0, 4});
    }
    r.engine->commit();
    /* The louder side: both of a clip's are the same here. */
    auto level = [&](int t) {
        float l = 0.0f, rr = 0.0f;
        r.engine->trackLevels(t, l, rr);
        CHECK(l == rr);
        return l;
    };
    CHECK(level(0) == 0.0f);         // stopped
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    REQUIRE(r.engine->play(0));
    REQUIRE(waitFor([&] { return r.engine->playing(); }));
    auto render = [&](int blocks) {
        for (int i = 0; i < blocks; ++i) {
            r.block(out, bus);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    };
    REQUIRE(waitFor([&] { render(1); return level(0) > 0.0f; }));
    render(20);
    CHECK_THAT(level(0), Catch::Matchers::WithinAbs(0.5, 0.01));
    CHECK_THAT(level(1), Catch::Matchers::WithinAbs(0.25, 0.01));
    CHECK(level(2) == 0.0f);

    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[1].solo = true;
        CHECK(r.engine->song().silenced(0));
        CHECK_FALSE(r.engine->song().silenced(1));
    }
    r.engine->commit();
    render(200);   // past what the ring already held, mixed before the solo
    CHECK(level(0) == 0.0f);
    CHECK_THAT(level(1), Catch::Matchers::WithinAbs(0.25, 0.01));
    float peak = 0.0f;
    for (float v : out) peak = std::max(peak, v);
    CHECK_THAT(peak, Catch::Matchers::WithinAbs(0.25, 0.01));
    r.engine->stop();
    REQUIRE(waitFor([&] { return !r.engine->playing(); }));
    CHECK(level(1) == 0.0f);
}

TEST_CASE("song engine: a missing clip plays silence and is counted", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        SongClip c;
        c.file = "gone.wav";
        c.bar = 0;
        c.bars = 1;
        r.engine->song().bpm = 120.0f;
        r.engine->song().addClip(0, c);
    }
    r.engine->commit();
    REQUIRE(r.engine->play(0));
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    REQUIRE(waitFor([&] {
        r.block(out, bus);
        return r.engine->openFailures() > 0;
    }));
    for (float v : out) CHECK(v == 0.0f);
}

TEST_CASE("song engine: bounce writes the pads bus to a clip on the track", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    r.seq.setSceneBars(0, 1);
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.25f);
    REQUIRE(r.engine->bounceScene(0, 0));
    /* Near real time (a block is 5.8 ms): the bounce ring holds ~0.7 s, so a
     * worker held off the CPU for a while on a loaded machine still keeps up. */
    for (int i = 0; i < 4000 && r.engine->bounceStatus().state == BounceState::Running; ++i) {
        r.block(out, bus);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    const BounceStatus st = r.engine->bounceStatus();
    REQUIRE(st.state == BounceState::Done);
    CHECK(r.platform.captureBegun == 1);
    CHECK(r.platform.captureEnded == 1);
    const fs::path wav = r.clipsDir("Demo") / st.file;
    REQUIRE(fs::exists(wav));
    /* The take is the pads bus, sample for sample: 0.25 of full scale, one bar. */
    const std::vector<int16_t> pcm = readWavData(wav.string());
    REQUIRE(pcm.size() == size_t(st.framesTotal) * 2);
    CHECK(st.framesDone == st.framesTotal);
    const int16_t expect = int16_t(0.25f * 32767.0f);
    CHECK(std::all_of(pcm.begin(), pcm.end(), [&](int16_t v) { return v == expect; }));
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    REQUIRE(r.engine->song().tracks[0].clips.size() == 1);
    CHECK(r.engine->song().tracks[0].clips[0].file == st.file);
}

TEST_CASE("song engine: a cancelled bounce leaves no file and restores the bus", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    r.seq.setSceneBars(0, 4);
    r.seq.setCountIn(true);
    const uint8_t sceneWas = r.seq.activeScene();
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.25f);
    REQUIRE(r.engine->bounceScene(0, 0));
    REQUIRE(waitFor([&] { return r.platform.captureBegun.load() == 1; }));
    CHECK_FALSE(r.seq.countIn());   // a bounce plays the scene without count-in
    for (int i = 0; i < 20; ++i) r.block(out, bus);
    r.engine->cancelBounce();
    REQUIRE(waitFor([&] { return r.engine->bounceStatus().state == BounceState::Failed; }));
    CHECK(r.engine->bounceStatus().error == "cancelled");
    CHECK(r.platform.captureEnded == r.platform.captureBegun);
    CHECK(fs::is_empty(r.clipsDir("Demo")));
    CHECK(r.seq.countIn());
    CHECK(r.seq.activeScene() == sceneWas);
}

namespace {

/// Frame @p k of a take as the pads bus carries it: (k % 30000, k / 30000),
/// so no two frames of a take are alike and a piece written to the wrong
/// place shows. Half a step up, as the capture truncates.
float rampSample(uint32_t v) { return ((float)v + 0.5f) / 32767.0f; }

/// Feeds a bounce the ramp, counting from the block its take starts on: the
/// block in which the clock starts (the capture's own test).
struct RampFeed {
    uint32_t next = 0;
    bool started = false;
    std::vector<float> out = std::vector<float>(kBlock * 2);
    std::vector<float> bus = std::vector<float>(kBlock * 2, 0.0f);

    void block(Rig& r) {
        std::fill(out.begin(), out.end(), 0.0f);
        r.clock.mix(out.data(), kBlock);
        r.seq.poll();
        std::fill(out.begin(), out.end(), 0.0f);
        r.engine->mix(out.data(), kBlock);
        const uint32_t at = r.clock.positionSamples();
        if (!started && r.clock.running() && !r.clock.countingIn() && at > 0 && at <= kBlock) started = true;
        for (uint32_t i = 0; i < kBlock; ++i) {
            const uint32_t k = started ? next++ : 0;
            bus[i * 2]     = rampSample(k % 30000);
            bus[i * 2 + 1] = rampSample(k / 30000);
        }
        r.engine->rtCapture(bus.data(), kBlock);
    }

    /// Blocks near real time until the bounce is no longer running.
    BounceStatus run(Rig& r) {
        for (int i = 0; i < 4000 && r.engine->bounceStatus().state == BounceState::Running; ++i) {
            block(r);
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        return r.engine->bounceStatus();
    }
};

std::vector<uint8_t> readFile(const fs::path& path) {
    std::vector<uint8_t> all;
    if (std::FILE* f = std::fopen(path.string().c_str(), "rb")) {
        uint8_t buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) all.insert(all.end(), buf, buf + n);
        std::fclose(f);
    }
    return all;
}

/// One bar at 120 BPM bounced from the ramp; the file's bytes and every
/// writeIo it took.
struct RampTake {
    BounceStatus status;
    std::vector<uint8_t> bytes;
    std::vector<std::pair<long, size_t>> writes;
};

RampTake bounceRamp(bool rawIo, SongEngineSizes sizes = {}) {
    Rig r(sizes);
    r.platform.rawIo = rawIo;
    REQUIRE(r.open("Demo"));
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.engine->bounceScene(0, 0));
    RampFeed feed;
    RampTake t;
    t.status = feed.run(r);
    REQUIRE(t.status.state == BounceState::Done);
    t.bytes = readFile(r.clipsDir("Demo") / t.status.file);
    t.writes = r.platform.writesSoFar();
    return t;
}

} // namespace

TEST_CASE("song engine: a bounce writes 4 kB pieces that end on 4 kB of the file", "[song][bounce]") {
    for (const bool rawIo : {true, false}) {
        CAPTURE(rawIo);
        const RampTake t = bounceRamp(rawIo);
        /* One bar at 120 BPM: 88200 frames = 1013 (up to the first 4 kB of
         * the file, after the 44-byte header) + 85 x 1024 + 147. */
        REQUIRE(t.status.framesTotal == 88200);
        REQUIRE(t.writes.size() == 87);
        CHECK(t.writes.front() == std::make_pair(44L, size_t(4052)));
        for (size_t i = 1; i + 1 < t.writes.size(); ++i) {
            CAPTURE(i);
            CHECK(t.writes[i] == std::make_pair(long(i * 4096), size_t(4096)));
        }
        CHECK(t.writes.back() == std::make_pair(86L * 4096, size_t(588)));
        CHECK(t.status.chunks == 87);
        CHECK(t.status.chunkMaxUs >= t.status.chunkAvgUs);
        CHECK(t.status.drainKBps > 0);

        /* The take is the ramp, frame for frame, across every piece's edge. */
        const std::vector<uint8_t>& b = t.bytes;
        REQUIRE(b.size() == 44 + 88200 * 4);
        auto le32 = [&](size_t at) { return b[at] | b[at + 1] << 8 | b[at + 2] << 16 | uint32_t(b[at + 3]) << 24; };
        CHECK(std::memcmp(b.data(), "RIFF", 4) == 0);
        CHECK(le32(4) == 36 + 88200 * 4);
        CHECK(std::memcmp(b.data() + 36, "data", 4) == 0);
        CHECK(le32(40) == 88200 * 4);
        size_t wrong = 0;
        for (uint32_t k = 0; k < 88200; ++k) {
            int16_t lr[2];
            std::memcpy(lr, b.data() + 44 + k * 4, 4);
            if (lr[0] != int16_t(k % 30000) || lr[1] != int16_t(k / 30000)) ++wrong;
        }
        CHECK(wrong == 0);
    }
}

TEST_CASE("song engine: a bounce takes its piece size from the platform", "[song][bounce]") {
    SongEngineSizes sizes;
    sizes.chunkFrames = 512;
    const RampTake t = bounceRamp(true, sizes);
    /* 88200 frames = 501 (up to the first 2 kB of the file, after the 44-byte
     * header) + 171 x 512 + 147. */
    REQUIRE(t.status.framesTotal == 88200);
    REQUIRE(t.writes.size() == 173);
    CHECK(t.writes.front() == std::make_pair(44L, size_t(2004)));
    for (size_t i = 1; i + 1 < t.writes.size(); ++i) {
        CAPTURE(i);
        CHECK(t.writes[i] == std::make_pair(long(i * 2048), size_t(2048)));
    }
    CHECK(t.writes.back() == std::make_pair(172L * 2048, size_t(588)));
    CHECK(t.bytes == bounceRamp(true).bytes);
}

TEST_CASE("song engine: a bounce past stdio writes the same file as through it", "[song][bounce]") {
    const RampTake raw = bounceRamp(true);
    const RampTake viaStdio = bounceRamp(false);
    REQUIRE(raw.bytes.size() == viaStdio.bytes.size());
    CHECK(raw.bytes == viaStdio.bytes);
}

TEST_CASE("song engine: a piece the card does not take fails the bounce and leaves no file", "[song][bounce]") {
    Rig r;
    r.platform.rawIo = true;
    r.platform.failWriteAt = 3;
    REQUIRE(r.open("Demo"));
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.engine->bounceScene(0, 0));
    RampFeed feed;
    const BounceStatus st = feed.run(r);
    REQUIRE(st.state == BounceState::Failed);
    CHECK(st.error == "card write failed");
    CHECK(st.chunks == 3);
    CHECK(r.platform.captureEnded == 1);
    CHECK(fs::is_empty(r.clipsDir("Demo")));
}

TEST_CASE("song engine: a bounce cancelled between pieces leaves no file", "[song][bounce]") {
    Rig r;
    r.platform.rawIo = true;
    REQUIRE(r.open("Demo"));
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.engine->bounceScene(0, 0));
    RampFeed feed;
    for (int i = 0; i < 2000 && r.platform.writesSoFar().size() < 4; ++i) {
        feed.block(r);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    REQUIRE(r.platform.writesSoFar().size() >= 4);
    r.engine->cancelBounce();
    const BounceStatus st = feed.run(r);
    REQUIRE(st.state == BounceState::Failed);
    CHECK(st.error == "cancelled");
    CHECK(r.platform.captureEnded == 1);
    CHECK(fs::is_empty(r.clipsDir("Demo")));
}

TEST_CASE("song engine: no storage, no project and no bounce", "[song]") {
    Rig r;
    r.platform.storage = false;
    REQUIRE(r.engine->requestOpen("Demo"));
    REQUIRE(waitFor([&] { return r.engine->openState() == SongOpState::Failed; }));
    REQUIRE(r.engine->bounceScene(0, 0));
    REQUIRE(waitFor([&] { return r.engine->bounceStatus().state == BounceState::Failed; }));
}

TEST_CASE("song engine: every clip opened for the list asks the platform first", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    for (const char* n : {"a.wav", "b.wav", "c.wav"}) {
        writeClip((r.clipsDir("Demo") / n).string(), kRate, 100);
    }
    const int before = r.platform.storageAsked.load();
    REQUIRE(r.engine->requestClips());
    REQUIRE(waitFor([&] { return r.engine->clips().size() == 3; }));
    /* The board's open aborts when internal RAM is short; the platform must
     * get the chance to say no before each one, not once per listing. */
    CHECK(r.platform.storageAsked.load() - before >= 4);
}

TEST_CASE("song engine: stop and play hammered while the render runs", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    writeClip((r.clipsDir("Demo") / "c.wav").string(), kRate * 4, 8000);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        SongClip c;
        c.file = "c.wav";
        c.bar = 0;
        c.bars = 2;
        r.engine->song().bpm = 120.0f;
        r.engine->song().addClip(0, c);
    }
    r.engine->commit();
    /* The render keeps going as it would on the audio thread; every stop makes
     * the worker free the ring it is reading from (rtGen_ is what makes that
     * safe). Run under ASan/TSan this is the use-after-free check. */
    std::atomic<bool> run{true};
    std::thread render([&] {
        std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
        while (run.load()) {
            r.block(out, bus);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });
    for (int i = 0; i < 40; ++i) {
        r.engine->play(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(3 + i % 5));
        r.engine->stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(i % 3));
    }
    REQUIRE(waitFor([&] { return !r.engine->playing(); }));
    run.store(false);
    render.join();
}

namespace {

/// A clip of @p bars bars at 120 BPM (2 s a bar) on @p track at @p bar.
void placeClip(Rig& r, const std::string& file, int16_t value, uint8_t track, uint32_t bar, uint16_t bars) {
    writeClip((r.clipsDir("Demo") / file).string(), kRate * 2 * bars, value);
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    AudioSong& s = r.engine->song();
    s.bpm = 120.0f;
    SongClip c;
    c.file = file;
    c.bar = bar;
    c.bars = bars;
    s.addClip(track, c);
    s.tracks[track].inUse = true;
}

bool editDone(Rig& r) {
    return waitFor([&] { return r.engine->editState() != SongOpState::Busy; }, 10000) &&
           r.engine->editState() == SongOpState::Ready;
}

} // namespace

TEST_CASE("song engine: merge mixes the tracks into one clip on the lowest", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    placeClip(r, "a.wav", 1000, 1, 0, 2);
    placeClip(r, "b.wav", 300, 2, 1, 1);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[2].volume = 0.5f;
    }
    r.engine->commit();
    REQUIRE(r.engine->requestMerge(0b0110, 0, 2));
    REQUIRE(editDone(r));
    CHECK(r.engine->editProgress() == 100);

    /* Bar 0 is track 1 alone, bar 1 both at their volumes: sample-exact. */
    const std::vector<int16_t> pcm = readWavData((r.clipsDir("Demo") / "merge_01.wav").string());
    REQUIRE(pcm.size() == size_t(kRate) * 2 * 2 * 2);
    CHECK(pcm.front() == 1000);
    CHECK(pcm[size_t(kRate) * 2 * 2 - 2] == 1000);
    CHECK(pcm[size_t(kRate) * 2 * 2] == 1150);
    CHECK(pcm.back() == 1150);

    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const AudioSong& s = r.engine->song();
    REQUIRE(s.tracks[1].clips.size() == 1);
    CHECK(s.tracks[1].clips[0].file == "merge_01.wav");
    CHECK(s.tracks[1].clips[0].bars == 2);
    CHECK(s.tracks[1].volume == 1.0f);
    CHECK(s.tracks[2].clips.empty());
}

TEST_CASE("song engine: merge leaves muted tracks out and cuts a block reaching in", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    placeClip(r, "long.wav", 500, 0, 0, 3);
    placeClip(r, "m.wav", 700, 1, 1, 1);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[1].muted = true;
    }
    r.engine->commit();
    REQUIRE(r.engine->requestMerge(0b0011, 1, 2));
    REQUIRE(editDone(r));
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const AudioSong& s = r.engine->song();
    /* long.wav keeps bar 0; the merge starts at bar 1 on track 0. */
    REQUIRE(s.tracks[0].clips.size() == 2);
    CHECK(s.tracks[0].clips[0].file == "long.wav");
    CHECK(s.tracks[0].clips[0].bars == 1);
    CHECK(s.tracks[0].clips[1].bar == 1);
    REQUIRE(s.tracks[1].clips.size() == 1);   // muted: untouched
    CHECK(s.tracks[1].clips[0].file == "m.wav");
}

TEST_CASE("song engine: merge keeps the volume of a track whose other blocks stay", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    placeClip(r, "a.wav", 1000, 1, 0, 1);
    placeClip(r, "keep.wav", 400, 1, 3, 1);
    placeClip(r, "b.wav", 300, 2, 0, 1);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[1].volume = 0.5f;
    }
    r.engine->commit();
    REQUIRE(r.engine->requestMerge(0b0110, 0, 1));
    REQUIRE(editDone(r));

    /* Heard before: 1000 * 0.5 + 300 * 1.0 = 800, played back at track 1's 0.5. */
    const std::vector<int16_t> pcm = readWavData((r.clipsDir("Demo") / "merge_01.wav").string());
    REQUIRE_FALSE(pcm.empty());
    CHECK(pcm.front() == 1600);

    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const AudioSong& s = r.engine->song();
    CHECK(s.tracks[1].volume == 0.5f);
    REQUIRE(s.tracks[1].clips.size() == 2);
    CHECK(s.tracks[1].clips[1].file == "keep.wav");
}

TEST_CASE("song engine: an edit asked for while another runs is refused", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    placeClip(r, "a.wav", 1000, 0, 0, 1);
    r.engine->commit();
    r.platform.holdIo.store(true);
    REQUIRE(r.engine->requestMerge(0b0001, 0, 1));
    CHECK(r.engine->editState() == SongOpState::Busy);
    CHECK_FALSE(r.engine->requestTempo(90.0f));
    CHECK_FALSE(r.engine->requestRename("Other"));
    CHECK_FALSE(r.engine->requestImport((r.clipsDir("Demo") / "a.wav").string(), 0, 0, 1, 0, 0));
    r.platform.holdIo.store(false);
    REQUIRE(editDone(r));
    REQUIRE(r.engine->requestTempo(90.0f));
    REQUIRE(editDone(r));
}

TEST_CASE("song engine: opening another project saves the edit still waiting", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    placeClip(r, "a.wav", 1000, 0, 0, 1);
    r.engine->commit();                             // saved only after kSaveDelayMs
    REQUIRE(r.open("Other"));
    REQUIRE(r.open("Demo"));
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    REQUIRE(r.engine->song().tracks[0].clips.size() == 1);
    CHECK(r.engine->song().tracks[0].clips[0].file == "a.wav");
}

TEST_CASE("song engine: a new tempo resizes every block to its file", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    placeClip(r, "c.wav", 100, 0, 0, 2);            // 4 s
    r.engine->commit();
    REQUIRE(r.engine->requestTempo(240.0f));        // 1 s a bar
    REQUIRE(editDone(r));
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    CHECK(r.engine->song().bpm == 240.0f);
    CHECK(r.engine->song().tracks[0].clips[0].bars == 4);
    CHECK_FALSE(r.engine->requestTempo(5.0f));
}

TEST_CASE("song engine: a stretched track keeps its bars and is fitted to a new tempo", "[song][stretch]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    placeClip(r, "c.wav", 1000, 0, 0, 2);           // two bars at 120
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[0].stretch = StretchMode::KeepPitch;
    }
    r.engine->commit();
    REQUIRE(r.engine->requestTempo(132.0f));
    REQUIRE(editDone(r));
    uint32_t spb = 0;
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        CHECK(r.engine->song().tracks[0].clips[0].bars == 2);   // the audio is fitted, not the block
        spb = r.engine->song().samplesPerBar(kRate);
    }
    const fs::path copy = r.clipsDir("Demo") / ".stretch" / ("c_0_0_" + std::to_string(2 * spb) + ".wav");
    REQUIRE(waitFor([&] { return fs::exists(copy) && r.engine->stretchWaiting() == 0; }, 10000));
    const auto pcm = readWavData(copy.string());
    CHECK(pcm.size() == (size_t)2 * spb * 2);
    CHECK(pcm[1000 * 2] == 1000);
    CHECK(pcm[(2 * spb - 1000) * 2 + 1] == 1000);
}

TEST_CASE("song engine: undo and redo walk the arrangement, a burst is one step", "[song][undo]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    CHECK(r.engine->undoSteps() == 0);
    CHECK_FALSE(r.engine->undo());
    placeClip(r, "a.wav", 1000, 0, 0, 2);
    r.engine->commit();                               // step 1: a block
    CHECK(r.engine->undoSteps() == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    for (int i = 1; i <= 3; ++i) {                    // step 2: nudged three times, at once
        {
            std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
            r.engine->song().tracks[0].clips[0].bar = (uint32_t)i;
        }
        r.engine->commit();
    }
    CHECK(r.engine->undoSteps() == 2);
    r.engine->commit();                               // nothing changed: no step
    CHECK(r.engine->undoSteps() == 2);

    auto bar0 = [&] {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        const auto& cs = r.engine->song().tracks[0].clips;
        return cs.empty() ? -1 : (int)cs[0].bar;
    };
    REQUIRE(bar0() == 3);
    REQUIRE(r.engine->undo());
    CHECK(bar0() == 0);                               // the whole burst back
    CHECK(r.engine->redoSteps() == 1);
    REQUIRE(r.engine->undo());
    CHECK(bar0() == -1);                              // the block gone again
    CHECK_FALSE(r.engine->undo());
    REQUIRE(r.engine->redo());
    CHECK(bar0() == 0);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        CHECK(r.engine->song().tracks[0].clips[0].file == "a.wav");
        CHECK(r.engine->song().name == "Demo");      // the project stays the project
    }
    /* A new edit after an undo: what could be redone is gone. */
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[0].muted = true;
    }
    r.engine->commit();
    CHECK(r.engine->redoSteps() == 0);
    CHECK_FALSE(r.engine->redo());
    /* Another project: no steps of the last one's. */
    REQUIRE(r.open("Other"));
    CHECK(r.engine->undoSteps() == 0);
    CHECK(r.engine->redoSteps() == 0);
}

TEST_CASE("song engine: a tempo change is undone, bars and all", "[song][undo]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    placeClip(r, "a.wav", 1000, 0, 0, 2);            // two bars at 120
    r.engine->commit();
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    REQUIRE(r.engine->requestTempo(60.0f));
    REQUIRE(editDone(r));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        REQUIRE(r.engine->song().bpm == 60.0f);
        REQUIRE(r.engine->song().tracks[0].clips[0].bars == 1);
    }
    REQUIRE(r.engine->undo());
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    CHECK(r.engine->song().bpm == 120.0f);
    CHECK(r.engine->song().tracks[0].clips[0].bars == 2);
}

TEST_CASE("song engine: a tempo turned while playing is counted at once, and fitted after stop", "[song][stretch]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    placeClip(r, "c.wav", 1000, 0, 0, 2);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[0].stretch = StretchMode::KeepPitch;
    }
    r.engine->commit();
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    REQUIRE(r.engine->play(0));
    REQUIRE(waitFor([&] { return r.engine->playing(); }));
    REQUIRE(r.engine->requestTempo(132.0f));
    /* While it plays, song_io makes no copy -- but the screen is told. */
    REQUIRE(waitFor([&] { r.block(out, bus); return r.engine->stretchWaiting() == 1; }));
    for (int i = 0; i < 50; ++i) r.block(out, bus);
    CHECK(r.engine->stretchWaiting() == 1);
    r.engine->stop();
    REQUIRE(waitFor([&] { return !r.engine->playing(); }));
    REQUIRE(waitFor([&] { return r.engine->stretchWaiting() == 0; }, 10000));
}

TEST_CASE("song engine: a repitched track keeps its bars and plays at the new speed", "[song][stretch]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    placeClip(r, "c.wav", 1000, 0, 0, 2);           // two bars at 120
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[0].stretch = StretchMode::Repitch;
    }
    r.engine->commit();
    REQUIRE(r.engine->requestTempo(100.0f));
    REQUIRE(editDone(r));
    uint32_t spb = 0;
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        CHECK(r.engine->song().tracks[0].clips[0].bars == 2);
        spb = r.engine->song().samplesPerBar(kRate);
    }
    const fs::path copy = r.clipsDir("Demo") / ".stretch" / ("c_0_0_" + std::to_string(2 * spb) + "r.wav");
    REQUIRE(waitFor([&] { return fs::exists(copy) && r.engine->stretchWaiting() == 0; }, 10000));
    const auto pcm = readWavData(copy.string());
    CHECK(pcm.size() == (size_t)2 * spb * 2);
    CHECK(pcm[1000 * 2] == 1000);
    CHECK(pcm[(2 * spb - 1000) * 2 + 1] == 1000);
}

TEST_CASE("song engine: the projects are the folders under the songs root, sorted", "[song][fs]") {
    test::FakeFileSystem fake;
    test::UseFileSystem use(fake);
    Rig r;
    fake.addDir(r.platform.root);
    fake.addDir(r.platform.root + "/Zeta");
    fake.addFile(r.platform.root + "/notes.txt");
    fake.addDir(r.platform.root + "/.peaks");
    fake.addDir(r.platform.root + "/Alpha");
    REQUIRE(r.engine->requestProjects());
    REQUIRE(waitFor([&] { return r.engine->projects().size() == 2; }));
    CHECK(r.engine->projects() == std::vector<std::string>{"Alpha", "Zeta"});
}

TEST_CASE("song engine: rename moves the folder and is remembered", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    REQUIRE(r.open("Other"));
    REQUIRE(r.engine->requestRename("Demo"));       // taken
    REQUIRE(waitFor([&] { return r.engine->editState() != SongOpState::Busy; }));
    CHECK(r.engine->editState() == SongOpState::Failed);
    REQUIRE(r.engine->requestRename("Fresh"));
    REQUIRE(editDone(r));
    CHECK(fs::is_directory(fs::path(r.platform.root) / "Fresh"));
    CHECK_FALSE(fs::exists(fs::path(r.platform.root) / "Other"));
    CHECK(r.platform.current == "Fresh");
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    CHECK(r.engine->song().name == "Fresh");
    CHECK_FALSE(r.engine->requestRename("a/b"));
}

TEST_CASE("song engine: delete removes the open project and opens another", "[song]") {
    Rig r;
    REQUIRE(r.open("Alpha"));
    REQUIRE(r.open("Demo"));
    placeClip(r, "c.wav", 100, 0, 0, 4);
    REQUIRE(fs::exists(r.clipsDir("Demo") / "c.wav"));
    REQUIRE(r.engine->requestDelete());
    REQUIRE(editDone(r));
    CHECK_FALSE(fs::exists(fs::path(r.platform.root) / "Demo"));
    CHECK(r.engine->openState() == SongOpState::Ready);
    CHECK(r.platform.current == "Alpha");
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        CHECK(r.engine->song().name == "Alpha");
    }
    /* The last one goes too: a new "Song 1" takes its place. */
    REQUIRE(r.engine->requestDelete());
    REQUIRE(editDone(r));
    CHECK_FALSE(fs::exists(fs::path(r.platform.root) / "Alpha"));
    CHECK(fs::is_directory(fs::path(r.platform.root) / "Song 1"));
    CHECK(r.platform.current == "Song 1");
    r.engine->requestProjects();
    REQUIRE(waitFor([&] { return r.engine->projects().size() == 1; }));
    CHECK(r.engine->projects().front() == "Song 1");
}

TEST_CASE("song engine: with a loop region playback goes round it", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    placeClip(r, "c.wav", 100, 0, 0, 4);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().loopStart = 1;
        r.engine->song().loopEnd = 2;
    }
    r.engine->commit();
    REQUIRE(r.engine->play(0));
    REQUIRE(waitFor([&] { return r.engine->playing() && r.engine->positionBars() >= 0.0; }));
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    /* Three bars of blocks: past the region's end twice over. */
    double maxPos = 0.0;
    bool wrapped = false;
    double last = 0.0;
    for (uint32_t i = 0; i < kRate * 2 * 3 / kBlock; ++i) {
        r.block(out, bus);
        const double pos = r.engine->positionBars();
        maxPos = std::max(maxPos, pos);
        if (pos + 0.5 < last) wrapped = true;
        last = pos;
        if (i % 16 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(wrapped);
    CHECK(maxPos < 2.0);
    CHECK(last >= 1.0);
    r.engine->stop();
}

namespace {

/// A stereo WAV whose left and right samples are both the frame's index.
void writeRamp(const std::string& path, uint32_t frames) {
    WavWriter w;
    REQUIRE(w.open(path.c_str(), kRate, 2));
    std::vector<int16_t> buf(frames * 2);
    for (uint32_t f = 0; f < frames; ++f) buf[f * 2] = buf[f * 2 + 1] = int16_t(f % 30000);
    REQUIRE(w.write(buf.data(), frames));
    REQUIRE(w.close());
}

} // namespace

TEST_CASE("song engine: import lays a region on a track after its lead", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    placeClip(r, "old.wav", 50, 1, 0, 4);           // reaches into the import: cut at its bar
    placeClip(r, "gone.wav", 60, 1, 3, 1);          // starts inside: replaced
    r.engine->commit();
    const std::string src = (r.dir / "take.wav").string();
    writeRamp(src, kRate * 3);
    const uint32_t lead = 1000, a = 500, b = kRate * 2 + 500;    // 2 s of ramp after the lead
    REQUIRE(r.engine->requestImport(src, a, b, 1, 2, lead));
    REQUIRE(editDone(r));

    const std::vector<int16_t> pcm = readWavData((r.clipsDir("Demo") / "take.wav").string());
    REQUIRE(pcm.size() == size_t(lead + (b - a)) * 2);
    CHECK(std::all_of(pcm.begin(), pcm.begin() + lead * 2, [](int16_t v) { return v == 0; }));
    bool exact = true;
    for (uint32_t f = 0; f < b - a && exact; ++f) exact = pcm[(lead + f) * 2] == int16_t((a + f) % 30000);
    CHECK(exact);

    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const auto& cs = r.engine->song().tracks[1].clips;
    REQUIRE(cs.size() == 2);
    CHECK(cs[0].file == "old.wav");
    CHECK(cs[0].bars == 2);
    CHECK(cs[1].file == "take.wav");
    CHECK(cs[1].bar == 2);
    CHECK(cs[1].bars == 2);                         // 2 s + lead at 2 s a bar: into the second bar
}

TEST_CASE("song engine: an import never writes over a clip of the same name", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    placeClip(r, "take.wav", 70, 0, 0, 1);
    r.engine->commit();
    const std::string src = (r.dir / "take.wav").string();
    writeRamp(src, kRate);
    REQUIRE(r.engine->requestImport(src, 0, 0, 3, 5, 0));
    REQUIRE(editDone(r));
    CHECK(readWavData((r.clipsDir("Demo") / "take.wav").string()).front() == 70);
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    /* Track 4 was asked for, but nobody has made 2-4: a track is made from
     * the lowest free one, so it lands on 2. */
    REQUIRE(r.engine->song().tracks[1].clips.size() == 1);
    CHECK(r.engine->song().tracks[1].inUse);
    CHECK(r.engine->song().tracks[1].clips[0].file == "take_2.wav");
    CHECK_FALSE(r.engine->requestImport(src, 0, 0, 4, 0, 0));      // no track 5
}

TEST_CASE("song engine: framesPlayed counts from the first block heard", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    placeClip(r, "c.wav", 100, 0, 0, 2);
    r.engine->commit();
    CHECK(r.engine->framesPlayed() == 0);
    REQUIRE(r.engine->play(0));
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    /* Nothing counts while it primes. */
    REQUIRE(waitFor([&] {
        r.block(out, bus);
        return r.engine->framesPlayed() > 0;
    }));
    CHECK(r.engine->framesPlayed() == kBlock);
    for (int i = 0; i < 9; ++i) r.block(out, bus);
    CHECK(r.engine->framesPlayed() == 10 * kBlock);
    r.engine->stop();
    REQUIRE(waitFor([&] { return !r.engine->playing(); }));
    CHECK(r.engine->framesPlayed() == 0);
}

TEST_CASE("song engine: alone, a count-in clicks and the song starts on the sample after", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    placeClip(r, "c.wav", 8000, 0, 0, 2);
    r.engine->commit();
    REQUIRE(r.engine->playCountedIn(0, SyncTake::None, 4));
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    uint64_t rendered = 0;
    float clickPeak = 0.0f;
    uint8_t firstLeft = 0;
    REQUIRE(waitFor([&] {
        r.block(out, bus);
        if (r.engine->countInBeatsLeft() && !firstLeft) firstLeft = r.engine->countInBeatsLeft();
        rendered += kBlock;
        if (!r.engine->framesPlayed()) for (float v : out) clickPeak = std::max(clickPeak, v);
        return r.engine->framesPlayed() > 0;
    }, 20000));
    /* 120 BPM at 44.1 kHz: a beat is 22050 frames, the count-in four of them;
     * counted from the first block that clicked. */
    const uint32_t beat = kRate / 2;
    CHECK(firstLeft == 4);
    CHECK(clickPeak > 0.1f);
    const uint32_t played = r.engine->framesPlayed();
    CHECK(played < kBlock);                       // started inside a block, not on its edge
    /* Blocks before the count-in began are priming: whatever they were, the
     * count-in plus what played is a whole number of blocks after them. */
    CHECK((4 * beat + played) % kBlock == 0);
    CHECK(r.engine->countInBeatsLeft() == 0);
    r.engine->stop();
}

TEST_CASE("song engine: in sync the metronome counts in and song and clock start together", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    placeClip(r, "c.wav", 8000, 0, 0, 2);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().syncSeq = true;
    }
    r.engine->commit();
    REQUIRE(r.engine->playCountedIn(1, SyncTake::None, 2));
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    bool sawCountIn = false;
    REQUIRE(waitFor([&] {
        r.block(out, bus);
        sawCountIn = sawCountIn || r.clock.countingIn();
        return r.engine->framesPlayed() > 0;
    }, 20000));
    CHECK(sawCountIn);
    /* The song started where the clock's loop did, on the same sample: from
     * bar 1 of the song, the clock's position is that bar plus what played. */
    const uint32_t played = r.engine->framesPlayed();
    const uint32_t loop = r.clock.loopSamples();
    const uint32_t bar = r.clock.samplesPerBeat() * r.clock.beatsPerBar();
    CHECK(played < kBlock);
    CHECK(r.clock.positionSamples() == (bar + played) % loop);
    r.block(out, bus);
    CHECK(r.seq.state() == SequencerState::Running);
    r.engine->stop();
}

TEST_CASE("song engine: export writes a Live project into the song's folder", "[song][liveset]") {
    Rig r;
    REQUIRE(r.open("Soul vibe"));
    writeClip((r.clipsDir("Soul vibe") / "a.wav").string(), kRate * 4, 1000);   // two bars at 120
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        s.addClip(0, {"a.wav", 0, 2});
        s.addClip(2, {"a.wav", 4, 2});
        s.addClip(3, {"gone.wav", 0, 1});
    }
    r.engine->commit();
    const fs::path project = fs::path(r.platform.root) / "Soul vibe";
    const fs::path als = project / "Soul vibe.als";

    REQUIRE(r.engine->requestExportLiveSet());
    REQUIRE(waitFor([&] { return r.engine->editState() != SongOpState::Busy; }));
    REQUIRE(r.engine->editState() == SongOpState::Ready);
    CHECK(fs::exists(project / "Ableton Project Info" / "Project12.cfg"));
    CHECK_FALSE(fs::exists(project / "Soul vibe.als.tmp"));
    const std::string xml = gunzip(als.string());
    CHECK(xml.find("<RelativePath Value=\"clips/a.wav\" />") != std::string::npos);
    size_t clips = 0;
    for (size_t at = xml.find("<AudioClip "); at != std::string::npos; at = xml.find("<AudioClip ", at + 1)) ++clips;
    CHECK(clips == 2);
    auto report = r.engine->editReport();
    REQUIRE(report.size() == 2);
    CHECK(report[0] == "SONGS/Soul vibe/Soul vibe.als, 2 clips");
    CHECK(report[1] == "gone.wav: not in clips/, left out");

    /* Exported again: the first one -- as good as one Live saved -- is kept. */
    REQUIRE(r.engine->requestExportLiveSet());
    REQUIRE(waitFor([&] { return r.engine->editState() != SongOpState::Busy; }));
    REQUIRE(r.engine->editState() == SongOpState::Ready);
    CHECK(gunzip((project / "Backup" / "Soul vibe [1].als").string()) == xml);
    CHECK(gunzip(als.string()) == xml);
    report = r.engine->editReport();
    REQUIRE(report.size() >= 2);
    CHECK(report[1] == "The one there before is in Backup/ as Soul vibe [1].als");

    /* The project is still the song's: the extra files are not clips. */
    REQUIRE(r.engine->requestClips());
    REQUIRE(waitFor([&] { return !r.engine->clips().empty(); }));
    CHECK(r.engine->clips().size() == 1);
}

TEST_CASE("song engine: a clip's waveform is read once and kept beside the song", "[song][peaks]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    writeClip((r.clipsDir("Demo") / "a.wav").string(), 5000, 16384);   // half scale
    ClipPeaks cp;
    CHECK_FALSE(r.engine->clipPeaks("a.wav", cp));
    REQUIRE(waitFor([&] { return r.engine->clipPeaks("a.wav", cp); }));
    CHECK(cp.hop == 1024);
    REQUIRE(cp.values);
    REQUIRE(cp.values->size() == 5);                       // 5000 frames, the last hop short
    for (uint8_t v : *cp.values) CHECK(v == 128);
    const fs::path pk = fs::path(r.platform.root) / "Demo" / ".peaks" / "a.wav.pk";
    REQUIRE(fs::exists(pk));
    CHECK(r.engine->framesPerBar() > 0);

    /* Not a clip at all: an empty waveform, not a question asked forever. */
    REQUIRE(waitFor([&] { return r.engine->clipPeaks("gone.wav", cp); }));
    REQUIRE(cp.values);
    CHECK(cp.values->empty());

    /* Opened again, the waveform comes from .peaks/ -- marked here so it shows. */
    {
        std::FILE* f = std::fopen(pk.string().c_str(), "r+b");
        REQUIRE(f);
        std::fseek(f, 16, SEEK_SET);
        std::fputc(7, f);
        std::fclose(f);
    }
    REQUIRE(r.open("Other"));
    (void)r.engine->clipPeaks("x.wav", cp);                // the cache follows the open project
    REQUIRE(r.open("Demo"));
    REQUIRE(waitFor([&] { return r.engine->clipPeaks("a.wav", cp); }));
    CHECK((*cp.values)[0] == 7);

    /* A WAV that changed is read again. */
    writeClip((r.clipsDir("Demo") / "a.wav").string(), 3000, 32767);
    REQUIRE(r.open("Other"));
    (void)r.engine->clipPeaks("x.wav", cp);
    REQUIRE(r.open("Demo"));
    REQUIRE(waitFor([&] { return r.engine->clipPeaks("a.wav", cp); }));
    REQUIRE(cp.values->size() == 3);
    CHECK((*cp.values)[0] == 255);
}

TEST_CASE("song engine: a waveform waits while the song plays", "[song][peaks]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    writeClip((r.clipsDir("Demo") / "c.wav").string(), kRate * 4, 1000);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().bpm = 120.0f;
        r.engine->song().addClip(0, {"c.wav", 0, 2});
    }
    r.engine->commit();
    REQUIRE(r.engine->play(0));
    REQUIRE(waitFor([&] { return r.engine->playing(); }));
    const uint32_t gen = r.engine->peaksGeneration();
    ClipPeaks cp;
    CHECK_FALSE(r.engine->clipPeaks("c.wav", cp));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(r.engine->peaksGeneration() == gen);
    r.engine->stop();
    REQUIRE(waitFor([&] { return r.engine->clipPeaks("c.wav", cp); }));
    CHECK(r.engine->peaksGeneration() != gen);
}

TEST_CASE("song engine: audio never lands on the seq track", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        REQUIRE(r.engine->song().addSeqTrack(0, SeqSource::Kit, 0));
    }
    const std::string src = (r.dir / "take.wav").string();
    writeRamp(src, kRate);
    CHECK_FALSE(r.engine->requestImport(src, 0, 0, 0, 0, 0));
    CHECK_FALSE(r.engine->bounceScene(0, 0));
}

TEST_CASE("song engine: a song that is only a seq track playing its scene is one pass of it long", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    CHECK(r.engine->playEndBars() == 0);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        REQUIRE(r.engine->song().addSeqTrack(0, SeqSource::Kit, 0));
    }
    CHECK(r.engine->playEndBars() == 0);     // a new track is empty
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[0].seqBlocks = false;   // from before blocks had notes
    }
    CHECK(r.engine->playEndBars() >= 1);
}

// ── takes onto the seq track (Arrange's REC and DUB) ──────────────────────

namespace {
/// Drives a take: starts it, renders up to song frames, hits pads there.
struct Take {
    Rig& r;
    std::vector<float> out = std::vector<float>(kBlock * 2);
    std::vector<float> bus = std::vector<float>(kBlock * 2, 0.0f);
    explicit Take(Rig& rig) : r(rig) {}
    bool start(uint32_t bar, SyncTake take) {
        if (!r.engine->recordSeq(bar, take, 0)) return false;
        return waitFor([&] { r.block(out, bus); return r.engine->framesPlayed() > 0; }, 20000);
    }
    /// Render until @p frames of the take have played.
    void to(uint32_t frames) { while (r.engine->framesPlayed() < frames) r.block(out, bus); }
    void hit(uint8_t pad, uint32_t at, uint32_t held) {
        to(at);
        r.seq.noteIn(pad, 100);
        to(at + held);
        r.seq.noteOut(pad);
    }
    bool stop() {
        r.engine->stop();
        return waitFor([&] { return r.engine->seqTake() == SyncTake::None && !r.engine->playing(); });
    }
};
constexpr uint32_t kTicksBar = AudioSong::kSeqPpq * 4;
} // namespace

TEST_CASE("song engine: REC from a bar lays a block of its own notes there", "[song][songtake]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    Take t(r);
    REQUIRE(t.start(4, SyncTake::Record));               // the playhead on bar five
    CHECK(r.engine->seqTake() == SyncTake::Record);
    const uint32_t bar = r.engine->framesPerBar();
    t.hit(3, bar + bar / 4, bar / 8);                     // the take's bar two, beat two
    t.to(bar + bar * 3 / 4);                              // STOP three beats into it
    REQUIRE(t.stop());
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const AudioSong& s = r.engine->song();
    /* No seq track yet: one was made, playing the kit. */
    REQUIRE(s.tracks[0].inUse);
    REQUIRE(s.tracks[0].kind == TrackKind::Seq);
    CHECK(s.tracks[0].source == SeqSource::Kit);
    REQUIRE(s.tracks[0].clips.size() == 1);
    const SongClip& c = s.tracks[0].clips[0];
    CHECK(c.bar == 4);
    CHECK(c.bars == 2);                                   // up to the bar it was stopped in
    REQUIRE(c.notes);
    REQUIRE(c.notes->notes.size() == 1);
    CHECK(c.notes->notes[0].tick == kTicksBar + AudioSong::kSeqPpq);
    CHECK(c.notes->notes[0].pad == 3);
    CHECK(c.notes->notes[0].len > 0);
}

TEST_CASE("song engine: a REC ends with the bar of its last note, wherever STOP came", "[song][songtake]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    Take t(r);
    REQUIRE(t.start(0, SyncTake::Record));
    const uint32_t bar = r.engine->framesPerBar();
    t.hit(1, 0, bar / 16);
    t.hit(2, 3 * bar + bar * 3 / 4, bar / 16);            // the last quarter of bar four
    t.to(5 * bar + bar / 2);                              // STOP half way through bar six
    REQUIRE(t.stop());
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const auto& cs = r.engine->song().tracks[0].clips;
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].bars == 4);
    CHECK(r.engine->playEndBars() == 4);                  // the song ends with it again
}

TEST_CASE("song engine: a second REC further on is a sequence of its own", "[song][songtake]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    const uint32_t bar = r.engine->framesPerBar();
    {
        Take t(r);
        REQUIRE(t.start(0, SyncTake::Record));
        t.hit(1, 0, bar / 16);
        t.hit(3, 3 * bar, bar / 16);                      // bar four
        t.to(4 * bar + bar / 2);
        REQUIRE(t.stop());
    }
    {
        Take t(r);
        REQUIRE(t.start(4, SyncTake::Record));            // the marker on bar five
        t.hit(2, bar / 2, bar / 16);
        t.to(2 * bar);                                    // a bar with nothing in it
        REQUIRE(t.stop());
    }
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const auto& cs = r.engine->song().tracks[0].clips;
    REQUIRE(cs.size() == 2);
    CHECK(cs[0].bar == 0);
    CHECK(cs[0].bars == 4);
    CHECK(cs[1].bar == 4);
    CHECK(cs[1].bars == 1);                               // its notes are all in its first bar
    REQUIRE(cs[0].notes);
    REQUIRE(cs[1].notes);
    CHECK(cs[0].notes != cs[1].notes);
    REQUIRE(cs[1].notes->notes.size() == 1);
    CHECK(cs[1].notes->notes[0].pad == 2);
    CHECK(cs[1].notes->notes[0].tick == kTicksBar / 2);
}

TEST_CASE("song engine: DUB adds to the block it plays over, and its copy stays as it was", "[song][songtake]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    const uint32_t bar = r.engine->framesPerBar();
    {
        Take t(r);
        REQUIRE(t.start(0, SyncTake::Record));
        t.hit(1, 0, bar / 16);
        t.to(bar);
        REQUIRE(t.stop());
    }
    SeqPatternPtr first;
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        REQUIRE(s.tracks[0].clips.size() == 1);
        first = s.tracks[0].clips[0].notes;
        SongClip copy = s.tracks[0].clips[0];             // DUP
        copy.bar = 2;
        s.addClip(0, copy);
    }
    r.engine->commit();
    {
        Take t(r);
        REQUIRE(t.start(0, SyncTake::Overdub));
        t.hit(5, bar / 2, bar / 16);
        t.to(bar - bar / 8);
        REQUIRE(t.stop());
    }
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const auto& cs = r.engine->song().tracks[0].clips;
    REQUIRE(cs.size() == 2);
    REQUIRE(cs[0].notes->notes.size() == 2);
    CHECK(cs[0].notes->notes[1].pad == 5);
    CHECK(cs[0].bars == 1);                               // a dub does not lengthen it
    CHECK(cs[1].notes == first);
}

TEST_CASE("song engine: a REC with nothing played changes nothing", "[song][songtake]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    const uint32_t bar = r.engine->framesPerBar();
    {
        Take t(r);
        REQUIRE(t.start(0, SyncTake::Record));
        t.hit(1, 0, bar / 16);
        t.to(2 * bar);
        REQUIRE(t.stop());
    }
    Take t(r);
    REQUIRE(t.start(0, SyncTake::Record));
    t.to(bar);
    REQUIRE(t.stop());
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const auto& cs = r.engine->song().tracks[0].clips;
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].notes->notes.size() == 1);
}

TEST_CASE("song engine: a REC longer than a scene lands as blocks the seq view opens", "[song][songtake][seqedit]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    const uint32_t bar = r.engine->framesPerBar();
    Take t(r);
    REQUIRE(t.start(0, SyncTake::Record));
    t.hit(2, bar / 2, bar / 16);                          // bar one
    t.hit(5, 9 * bar + bar / 4, bar / 16);                // bar ten
    t.to(10 * bar + bar / 2);                             // STOP half way into bar eleven, past it
    REQUIRE(t.stop());
    uint8_t scene = 0;
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        const auto& tr = r.engine->song().tracks[0];
        scene = tr.scene;
        REQUIRE(tr.clips.size() == 2);
        CHECK(tr.clips[0].bar == 0);
        CHECK(tr.clips[0].bars == AudioSong::kSeqBlockBars);
        CHECK(tr.clips[1].bar == AudioSong::kSeqBlockBars);
        CHECK(tr.clips[1].bars == 2);                     // to bar ten, its last note's
    }
    /* Each opens in the seq view, its own notes in the scene. */
    REQUIRE(r.engine->openSeqEdit(0, 0, scene));
    CHECK(r.seq.eventCount(scene) == 1);
    REQUIRE(r.engine->openSeqEdit(0, 9, scene));
    CHECK(r.engine->seqEditBar() == AudioSong::kSeqBlockBars);
    CHECK(r.seq.sceneBars(scene) == 2);
    CHECK(r.seq.eventCount(scene) == 1);
    r.engine->closeSeqEdit();
}

TEST_CASE("song engine: a take is one step of undo", "[song][songtake][undo]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    const uint32_t bar = r.engine->framesPerBar();
    Take t(r);
    REQUIRE(t.start(0, SyncTake::Record));
    t.hit(1, 0, bar / 16);
    t.to(bar);
    REQUIRE(t.stop());
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        REQUIRE(r.engine->song().tracks[0].clips.size() == 1);
    }
    REQUIRE(r.engine->undo());
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        CHECK(r.engine->song().tracks[0].clips.empty());
    }
    REQUIRE(r.engine->redo());
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    REQUIRE(r.engine->song().tracks[0].clips.size() == 1);
    CHECK(r.engine->song().tracks[0].clips[0].notes->notes.size() == 1);
}

TEST_CASE("song engine: REC round a region replaces it once, then dubs each lap", "[song][songtake]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    struct Count { std::atomic<int> pad7{0}; } heard;
    r.seq.setSink([](void* ctx, uint8_t pad, uint8_t, bool on) {
        if (on && pad == 7) ++static_cast<Count*>(ctx)->pad7;
    }, &heard);
    const uint32_t bar = r.engine->framesPerBar();
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        s.loop = true;
        s.loopStart = 2;
        s.loopEnd = 3;
    }
    r.engine->commit();
    Take t(r);
    REQUIRE(t.start(0, SyncTake::Record));                // starts at the region's bar, not 0
    t.hit(7, bar / 4, bar / 16);                          // lap one, beat two
    t.to(bar + bar / 8);                                  // into lap two
    /* Lap one landed and the track is heard again: its note plays in lap two. */
    REQUIRE(waitFor([&] {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        return r.engine->song().tracks[0].clips.size() == 1;
    }));
    const int before = heard.pad7.load();
    t.hit(9, bar + bar / 2, bar / 16);                    // lap two, beat three
    CHECK(heard.pad7.load() > before);
    t.to(bar + bar * 7 / 8);
    REQUIRE(t.stop());
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const auto& cs = r.engine->song().tracks[0].clips;
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].bar == 2);
    CHECK(cs[0].bars == 1);
    REQUIRE(cs[0].notes->notes.size() == 2);
    CHECK(cs[0].notes->notes[0].pad == 7);
    CHECK(cs[0].notes->notes[0].tick == AudioSong::kSeqPpq);
    CHECK(cs[0].notes->notes[1].pad == 9);
    CHECK(cs[0].notes->notes[1].tick == 2 * AudioSong::kSeqPpq);
}

TEST_CASE("song engine: a take punched in while playing starts on the bar it is in", "[song][songtake]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    const uint32_t bar = r.engine->framesPerBar();
    {
        Take t(r);
        REQUIRE(t.start(0, SyncTake::Record));
        t.hit(1, 0, bar / 16);
        t.hit(1, 3 * bar, bar / 16);                      // four bars long
        t.to(4 * bar);
        REQUIRE(t.stop());
    }
    Take t(r);
    REQUIRE(r.engine->play(0));
    REQUIRE(waitFor([&] { t.r.block(t.out, t.bus); return r.engine->framesPlayed() > 0; }, 20000));
    t.to(bar + bar / 2);
    REQUIRE(r.engine->seqPunchable());
    REQUIRE(r.engine->seqPunch(SyncTake::Record));
    t.hit(4, 2 * bar + bar / 4, bar / 16);
    t.to(3 * bar - bar / 8);
    REQUIRE(r.engine->seqPunch(SyncTake::None));         // out, and playing on
    CHECK(r.engine->playing());
    r.engine->stop();
    REQUIRE(waitFor([&] { return !r.engine->playing(); }));
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const auto& cs = r.engine->song().tracks[0].clips;
    /* Bars 1..2 replaced: the first block cut round them, its tail on. */
    REQUIRE(cs.size() == 3);
    CHECK(cs[0].bar == 0);
    CHECK(cs[0].bars == 1);
    CHECK(cs[1].bar == 1);
    CHECK(cs[1].bars == 2);
    CHECK(cs[2].bar == 3);
    REQUIRE(cs[1].notes->notes.size() == 1);
    CHECK(cs[1].notes->notes[0].pad == 4);
    CHECK(cs[1].notes->notes[0].tick == kTicksBar + AudioSong::kSeqPpq);
}

TEST_CASE("song engine: a track made for a take holds the take, not the scene up", "[song][songtake]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, 0, 8, 100));            // a groove in the scene that is up
    const uint32_t bar = r.engine->framesPerBar();
    Take t(r);
    REQUIRE(t.start(2, SyncTake::Record));
    t.hit(1, 0, bar / 16);
    t.to(bar);
    REQUIRE(t.stop());
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const auto& cs = r.engine->song().tracks[0].clips;
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].bar == 2);
    REQUIRE(cs[0].notes->notes.size() == 1);
    CHECK(cs[0].notes->notes[0].pad == 1);
}

TEST_CASE("song engine: a track that played its scene keeps it around a take", "[song][songtake]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, 0, 8, 100));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        s.length = 4;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
        s.tracks[0].seqBlocks = false;                     // the scene over the whole song
    }
    r.engine->commit();
    const uint32_t bar = r.engine->framesPerBar();
    Take t(r);
    REQUIRE(t.start(1, SyncTake::Record));
    t.hit(2, 0, bar / 16);
    t.to(bar);
    REQUIRE(t.stop());
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const auto& cs = r.engine->song().tracks[0].clips;
    /* Bar 0 and bars 2..3 still the scene's notes, bar 1 the take's. */
    REQUIRE(cs.size() == 3);
    CHECK(cs[0].bar == 0);
    CHECK(cs[0].notes->notes[0].pad == 8);
    CHECK(cs[1].bar == 1);
    CHECK(cs[1].notes->notes[0].pad == 2);
    CHECK(cs[2].bar == 2);
    CHECK(cs[2].notes == cs[0].notes);
}

TEST_CASE("song engine: a song opened lets the last one's notes go", "[song][seq]") {
    /* An element entered and left is a song opened each way, its notes read
     * anew: what the engine kept of the one before is never asked for again,
     * and must not keep it alive. */
    Rig r;
    REQUIRE(r.open("Demo"));
    std::weak_ptr<const SeqPattern> was;
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 1));
        SeqNote n;
        n.tick = 0; n.pad = 2; n.vel = 100;
        const SeqPatternPtr notes = makeSeqPattern(1, {n});
        was = notes;
        REQUIRE(s.putSeqTake(0, 0, notes, s.samplesPerBar(kRate)) == 0);
    }
    r.engine->commit();                                    // laid on the sequencer: its notes as samples
    REQUIRE(r.open("Other"));
    CHECK(was.expired());
}

// ── the seq view of a block ────────────────────────────────────────────────

TEST_CASE("song engine: the seq view edits one block, and its copy keeps its notes", "[song][seqedit]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    SeqPatternPtr first;
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 1));
        SeqNote n;
        n.tick = 0; n.pad = 2; n.vel = 100;
        first = makeSeqPattern(2, {n});
        s.putSeqTake(0, 4, first, s.samplesPerBar(kRate));
        SongClip copy = s.tracks[0].clips[0];
        copy.bar = 8;
        s.addClip(0, copy);
    }
    r.engine->commit();
    while (r.seq.sceneCount() < 2) r.seq.addScene();
    REQUIRE(r.engine->openSeqEdit(0, 5, 1));               // a bar inside the block on 4
    CHECK(r.engine->seqEditBar() == 4);
    REQUIRE(r.seq.sceneBars(1) == 2);
    REQUIRE(r.seq.eventCount(1) == 1);
    r.engine->syncSeqEdit();                               // nothing changed: nothing written
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        CHECK(r.engine->song().tracks[0].clips[0].notes == first);
    }
    REQUIRE(r.seq.addEventTicks(1, AudioSong::kSeqPpq * 2, 6, 90));
    r.seq.markDirty();                                     // as a take or an erase does
    r.engine->syncSeqEdit();
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const auto& cs = r.engine->song().tracks[0].clips;
    REQUIRE(cs.size() == 2);
    REQUIRE(cs[0].notes->notes.size() == 2);
    CHECK(cs[0].notes->notes[1].pad == 6);
    CHECK(cs[0].notes->notes[1].tick == AudioSong::kSeqPpq * 2);
    CHECK(cs[1].notes == first);
}

TEST_CASE("song engine: the seq view left while its REC still runs puts the take on the block", "[song][seqedit]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 1));
        SeqNote n;
        n.tick = 0; n.pad = 2; n.vel = 100;
        s.putSeqTake(0, 0, makeSeqPattern(1, {n}), s.samplesPerBar(kRate));
    }
    r.engine->commit();
    while (r.seq.sceneCount() < 2) r.seq.addScene();
    REQUIRE(r.engine->openSeqEdit(0, 0, 1));
    REQUIRE(r.seq.eventCount(1) == 1);

    /* REC in the seq view, a hit, and ARR before STOP: the take is still on
     * when the view lets go of the block. */
    r.seq.setCountIn(false);
    r.seq.record();
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    REQUIRE(waitFor([&] { r.block(out, bus); return r.seq.isRecording(); }, 20000));
    REQUIRE(r.seq.addEventTicks(1, AudioSong::kSeqPpq * 2, 6, 90));
    r.seq.markDirty();
    r.engine->syncSeqEdit();                               // while it records: left alone
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        CHECK(r.engine->song().tracks[0].clips[0].notes->notes.size() == 1);
    }
    r.engine->closeSeqEdit();
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    /* The hit is on the block -- what REC replaced of the old notes as it
     * went past them is the take's business. */
    const auto& c = r.engine->song().tracks[0].clips[0];
    bool hit = false;
    for (const auto& n : c.notes->notes) hit = hit || (n.pad == 6 && n.tick == AudioSong::kSeqPpq * 2);
    CHECK(hit);
}

TEST_CASE("song engine: the seq view on an empty bar makes a block of its first notes", "[song][seqedit]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
        SeqNote n;
        n.pad = 1; n.vel = 100;
        s.putSeqTake(0, 6, makeSeqPattern(1, {n}), s.samplesPerBar(kRate));
    }
    r.engine->commit();
    REQUIRE(r.engine->openSeqEdit(0, 3, 0));
    CHECK(r.seq.eventCount(0) == 0);
    r.engine->syncSeqEdit();                               // empty: no block yet
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        REQUIRE(r.engine->song().tracks[0].clips.size() == 1);
    }
    REQUIRE(r.seq.addEventTicks(0, 0, 4, 100));
    r.seq.markDirty();                                     // as a take or an erase does
    r.engine->syncSeqEdit();
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        const auto& cs = r.engine->song().tracks[0].clips;
        REQUIRE(cs.size() == 2);
        CHECK(cs[0].bar == 3);
        CHECK(cs[0].bars == 3);                            // four bars of scene, but bar 6 is taken
        CHECK(cs[0].notes->notes[0].pad == 4);
    }
    /* x2 in the seq view: a block as long as its notes follows them. */
    REQUIRE(r.seq.addEventTicks(0, AudioSong::kSeqPpq, 5, 100));
    r.seq.markDirty();                                     // as a take or an erase does
    r.engine->closeSeqEdit();
    CHECK(r.engine->seqEditBar() == -1);
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    CHECK(r.engine->song().tracks[0].clips[0].notes->notes.size() == 2);
}

TEST_CASE("song engine: a block as long as its notes follows the seq view's x2", "[song][seqedit]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
        SeqNote n;
        n.pad = 1; n.vel = 100;
        s.putSeqTake(0, 0, makeSeqPattern(2, {n}), s.samplesPerBar(kRate));
    }
    r.engine->commit();
    REQUIRE(r.engine->openSeqEdit(0, 0, 0));
    REQUIRE(r.seq.doubleScene(0));
    r.engine->syncSeqEdit();
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const auto& c = r.engine->song().tracks[0].clips[0];
    CHECK(c.bars == 4);
    CHECK(c.notes->bars == 4);
    CHECK(c.notes->notes.size() == 2);
}


TEST_CASE("song engine: a one-bar REC round a longer region goes round the whole region", "[song][songtake]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    const uint32_t bar = r.engine->framesPerBar();
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        s.loop = true;
        s.loopStart = 0;
        s.loopEnd = 4;
    }
    r.engine->commit();
    Take t(r);
    REQUIRE(t.start(0, SyncTake::Record));
    t.hit(2, 0, bar / 16);
    t.hit(3, bar / 2, bar / 16);                          // all of it in bar one
    t.to(bar + bar / 4);
    REQUIRE(t.stop());
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const auto& cs = r.engine->song().tracks[0].clips;
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].bar == 0);
    CHECK(cs[0].bars == 4);                               // the region, not one bar and silence
    REQUIRE(cs[0].notes);
    CHECK(cs[0].notes->bars == 1);                        // the bar, four times round
    CHECK(cs[0].notes->notes.size() == 2);
}

TEST_CASE("song engine: a short REC in a looping song plays on to the song's end", "[song][songtake]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    const uint32_t bar = r.engine->framesPerBar();
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        s.loop = true;
        REQUIRE(s.addAudioTrack(0));
        SongClip a;
        a.file = "a.wav";
        a.bars = 8;
        s.addClip(0, a);
    }
    r.engine->commit();
    Take t(r);
    REQUIRE(t.start(0, SyncTake::Record));
    t.hit(1, 0, bar / 16);
    t.hit(4, bar + bar / 2, bar / 16);                    // two bars of notes
    t.to(2 * bar + bar / 4);
    REQUIRE(t.stop());
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const auto& s = r.engine->song();
    REQUIRE(s.tracks[1].kind == TrackKind::Seq);
    const auto& cs = s.tracks[1].clips;
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].bars == 8);
    REQUIRE(cs[0].notes);
    CHECK(cs[0].notes->bars == 2);
}

TEST_CASE("song engine: x2 past a block its notes go round in lengthens the block", "[song][seqedit]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
        SeqNote n;
        n.pad = 1; n.vel = 100;
        s.putSeqTake(0, 0, makeSeqPattern(1, {n}), s.samplesPerBar(kRate), 2);   // a bar, twice round
    }
    r.engine->commit();
    REQUIRE(r.engine->openSeqEdit(0, 0, 0));
    REQUIRE(r.seq.doubleScene(0));                        // 2 bars: as long as the block
    r.engine->syncSeqEdit();
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        CHECK(r.engine->song().tracks[0].clips[0].bars == 2);
    }
    REQUIRE(r.seq.doubleScene(0));                        // 4 bars: longer than the block
    r.engine->syncSeqEdit();
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const auto& c = r.engine->song().tracks[0].clips[0];
    CHECK(c.bars == 4);
    CHECK(c.notes->bars == 4);
}

TEST_CASE("song engine: a seq track's bounce takes only the bars its blocks play", "[song][bounce]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        s.length = 16;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
        SeqNote n;
        n.pad = 1; n.vel = 100;
        s.putSeqTake(0, 5, makeSeqPattern(1, {n}), s.samplesPerBar(kRate));
    }
    r.engine->commit();
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.25f);
    REQUIRE(r.engine->bounceSeqTrack(0));
    for (int i = 0; i < 4000 && r.engine->bounceStatus().state == BounceState::Running; ++i) {
        r.block(out, bus);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    const BounceStatus st = r.engine->bounceStatus();
    REQUIRE(st.state == BounceState::Done);
    /* The bar and a bar of what rings on into the gap after it, not sixteen:
     * rendered as a freeze is (core 2.14). */
    CHECK(st.framesTotal == 2 * r.engine->framesPerBar());
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const auto& tr = r.engine->song().tracks[0];
    CHECK(tr.kind == TrackKind::Audio);
    CHECK(tr.freeze.empty());
    REQUIRE(tr.clips.size() == 1);
    CHECK(tr.clips[0].bar == 5);                          // where the block was
    CHECK(tr.clips[0].file == st.file);
}

TEST_CASE("song engine: a frozen track bounces with nothing rendered", "[song][bounce][freeze]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        s.length = 16;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
        SeqNote n;
        n.pad = 1; n.vel = 100;
        s.putSeqTake(0, 0, makeSeqPattern(2, {n}), s.samplesPerBar(kRate));
        s.tracks[0].clips[0].bars = 16;
        s.armed = -1;
    }
    r.engine->commit();
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.25f);
    auto run = [&](bool bounce) {
        const bool asked = bounce ? r.engine->bounceSeqTrack(0) : r.engine->freezeTrack(0);
        REQUIRE(asked);
        for (int i = 0; i < 6000 && r.engine->bounceStatus().state == BounceState::Running; ++i) {
            r.block(out, bus);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE(r.engine->bounceStatus().state == BounceState::Done);
    };
    run(false);
    std::vector<SongClip> frozen;
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        frozen = r.engine->song().tracks[0].freeze;
    }
    const int before = r.platform.captureBegun;
    run(true);
    CHECK(r.platform.captureBegun == before);              // every render was on the card
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const auto& tr = r.engine->song().tracks[0];
    CHECK(tr.kind == TrackKind::Audio);
    REQUIRE(tr.clips.size() == frozen.size());
    for (size_t i = 0; i < frozen.size(); ++i) {
        CHECK(tr.clips[i].file == frozen[i].file);
        CHECK(tr.clips[i].bar == frozen[i].bar);
        CHECK(tr.clips[i].offset == frozen[i].offset);
    }
}


TEST_CASE("song engine: a frozen seq track keeps its notes and plays its freeze while not armed", "[song][freeze]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        s.length = 16;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
        SeqNote n;
        n.pad = 1; n.vel = 100;
        s.putSeqTake(0, 5, makeSeqPattern(1, {n}), s.samplesPerBar(kRate));
        s.armed = -1;
    }
    r.engine->commit();
    CHECK(r.engine->freezeState(0) == FreezeState::None);
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.25f);
    REQUIRE(r.engine->freezeTrack(0));
    CHECK(r.engine->freezeState(0) == FreezeState::Freezing);
    for (int i = 0; i < 4000 && r.engine->bounceStatus().state == BounceState::Running; ++i) {
        r.block(out, bus);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    const BounceStatus st = r.engine->bounceStatus();
    REQUIRE(st.state == BounceState::Done);
    /* The bar, and a bar of what rings on into the gap after it; the bar
     * before it played, not recorded. */
    CHECK(st.framesTotal == 2 * r.engine->framesPerBar());
    CHECK(r.platform.routeCaptured == 0);                 // the kit's engine alone
    CHECK(r.engine->freezeState(0) == FreezeState::Frozen);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        const auto& tr = r.engine->song().tracks[0];
        CHECK(tr.kind == TrackKind::Seq);                 // still the Drums track
        REQUIRE(tr.clips.size() == 1);
        CHECK(tr.clips[0].notes);
        REQUIRE(tr.freeze.size() == 1);
        CHECK(tr.freeze[0].file == st.file);
        CHECK(tr.freeze[0].bar == 5);
        CHECK(tr.freeze[0].bars == 2);
        CHECK(tr.freeze[0].offset == 0);
    }
    CHECK(segmentsOf(*r.engine, 0) == 1);                 // its file plays

    SECTION("armed, it plays live") {
        {
            std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
            r.engine->song().armed = 0;
        }
        r.engine->commit();
        CHECK(segmentsOf(*r.engine, 0) == 0);
        CHECK(r.engine->freezeState(0) == FreezeState::Frozen);
    }
    SECTION("its notes or the tempo moved: stale, live, and frozen again once back") {
        {
            std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
            r.engine->song().bpm = 100.0f;
        }
        r.engine->commit();
        CHECK(r.engine->freezeState(0) == FreezeState::Stale);
        CHECK(segmentsOf(*r.engine, 0) == 0);
        {
            std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
            r.engine->song().bpm = 120.0f;
        }
        r.engine->commit();
        CHECK(r.engine->freezeState(0) == FreezeState::Frozen);
    }
    SECTION("kept in song.json") {
        std::string dir;
        {
            std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
            dir = r.engine->song().dir;
        }
        AudioSong back;
        REQUIRE(back.load(dir) == SongLoad::Loaded);
        REQUIRE(back.tracks[0].freeze.size() == 1);
        CHECK(back.tracks[0].freeze[0].file == st.file);
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        CHECK(back.tracks[0].freezeKey == r.engine->song().tracks[0].freezeKey);
    }
}

TEST_CASE("song engine: a block going round is frozen from two of its passes", "[song][freeze]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        s.length = 16;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
        SeqNote n;
        n.pad = 1; n.vel = 100;
        s.putSeqTake(0, 0, makeSeqPattern(2, {n}), s.samplesPerBar(kRate));
        s.tracks[0].clips[0].bars = 16;                   // a 2-bar take round the whole song
        s.armed = -1;
    }
    r.engine->commit();
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.25f);
    REQUIRE(r.engine->freezeTrack(0));
    for (int i = 0; i < 4000 && r.engine->bounceStatus().state == BounceState::Running; ++i) {
        r.block(out, bus);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const BounceStatus st = r.engine->bounceStatus();
    REQUIRE(st.state == BounceState::Done);
    const uint32_t fpb = r.engine->framesPerBar();
    CHECK(st.framesTotal == 4 * fpb);                     // two passes, not sixteen bars
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const auto& fz = r.engine->song().tracks[0].freeze;
    REQUIRE(fz.size() == 8);
    CHECK(fz[0].bar == 0);
    CHECK(fz[0].bars == 2);
    CHECK(fz[0].offset == 0);                             // the first pass, as it starts
    for (size_t i = 1; i < fz.size(); ++i) {
        CHECK(fz[i].file == fz[0].file);
        CHECK(fz[i].bar == 2 * i);
        CHECK(fz[i].bars == 2);
        CHECK(fz[i].offset == 2 * fpb);                   // every later pass: the second
    }
    CHECK(segmentsOf(*r.engine, 0) == 8);
}

TEST_CASE("song engine: a freeze renders a block once however often it recurs, and only what changed",
          "[song][freeze]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    SeqNote x, y;
    x.pad = 1; x.vel = 100;
    y.pad = 2; y.vel = 90;
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        s.length = 16;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
        const uint32_t spb = s.samplesPerBar(kRate);
        s.putSeqTake(0, 2, makeSeqPattern(2, {x}), spb);   // X, a bar of silence before and after
        s.putSeqTake(0, 6, makeSeqPattern(2, {x}), spb);   // X again, in the same surroundings
        s.putSeqTake(0, 10, makeSeqPattern(2, {y}), spb);  // Y
        s.armed = -1;
    }
    r.engine->commit();
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.25f);
    auto freeze = [&]() {
        REQUIRE(r.engine->freezeTrack(0));
        for (int i = 0; i < 6000 && r.engine->bounceStatus().state == BounceState::Running; ++i) {
            r.block(out, bus);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const BounceStatus st = r.engine->bounceStatus();
        REQUIRE(st.state == BounceState::Done);
        CHECK(st.pieces == 3);
        CHECK(st.piecesDone == 3);
        CHECK(r.engine->freezeState(0) == FreezeState::Frozen);
    };
    const int before = r.platform.captureBegun;
    freeze();
    CHECK(r.platform.captureBegun - before == 2);          // the second X is the first's file
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        const auto& fz = r.engine->song().tracks[0].freeze;
        REQUIRE(fz.size() == 3);
        CHECK(fz[0].file == fz[1].file);
        CHECK(fz[2].file != fz[0].file);
        CHECK(fz[0].file.rfind(".freeze/FRZ", 0) == 0);
    }

    SECTION("frozen again unchanged: nothing rendered") {
        const int was = r.platform.captureBegun;
        freeze();
        CHECK(r.platform.captureBegun == was);
    }
    SECTION("Y's notes changed: only Y rendered, X kept") {
        std::string xFile;
        {
            std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
            auto& s = r.engine->song();
            xFile = s.tracks[0].freeze[0].file;
            SeqNote z = y;
            z.vel = 40;
            s.tracks[0].clips[2].notes = makeSeqPattern(2, {z});
        }
        r.engine->commit();
        CHECK(r.engine->freezeState(0) == FreezeState::Stale);
        const int was = r.platform.captureBegun;
        freeze();
        CHECK(r.platform.captureBegun - was == 1);
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        const auto& fz = r.engine->song().tracks[0].freeze;
        CHECK(fz[0].file == xFile);
        CHECK(fz[1].file == xFile);
    }
}

TEST_CASE("song engine: PLAY or REC during a freeze cancels it and plays", "[song][freeze]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        s.length = 16;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
        SeqNote n;
        n.pad = 1; n.vel = 100;
        s.putSeqTake(0, 0, makeSeqPattern(4, {n}), s.samplesPerBar(kRate));
        s.armed = -1;
    }
    r.engine->commit();
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.25f);
    REQUIRE(r.engine->freezeTrack(0));
    for (int i = 0; i < 20; ++i) {
        r.block(out, bus);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(r.engine->bounceStatus().state == BounceState::Running);
    SECTION("PLAY") {
        REQUIRE(r.engine->play(0));
    }
    SECTION("REC onto another track (a new track armed: what set the freeze off)") {
        {
            std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
            REQUIRE(r.engine->song().addSeqTrack(1, SeqSource::Pitch, 3));
            r.engine->song().armed = 1;
        }
        r.engine->commit();
        REQUIRE(r.engine->recordSeq(0, SyncTake::Record, 0));
    }
    CHECK(r.engine->bounceStatus().state == BounceState::Failed);   // the freeze let go first
    REQUIRE(waitFor([&] {
        r.block(out, bus);
        return r.engine->playing();
    }));
    CHECK(r.engine->freezeState(0) == FreezeState::None);
    CHECK(segmentsOf(*r.engine, 0) == 0);                 // live
    r.engine->stop();
    REQUIRE(waitFor([&] { return !r.engine->playing(); }));
}


// ── two kit tracks, a kit each ───────────────────────────────────────────

namespace {
/// Every press the sequencer routes while a song plays: {route, pad}.
struct RoutedHits {
    std::mutex m;
    std::vector<std::pair<uint8_t, uint8_t>> ons;
    static void sink(void* ctx, uint8_t route, uint8_t pad, uint8_t, bool on, PadPerformance) {
        auto* h = static_cast<RoutedHits*>(ctx);
        std::lock_guard<std::mutex> l(h->m);
        if (on) h->ons.push_back({route, pad});
    }
};
} // namespace

TEST_CASE("song engine: REC takes onto the armed kit track, and each track plays on its own route", "[song][songtake][route]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    const uint32_t bar = r.engine->framesPerBar();
    {
        Take t(r);
        REQUIRE(t.start(0, SyncTake::Record));              // makes track 1, the pads' kit
        t.hit(1, 0, bar / 16);
        t.to(bar);
        REQUIRE(t.stop());
    }
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 2, "/crosspad/kits/B/kit.json"));   // its own scene
        s.armed = 1;
    }
    r.engine->commit();
    {
        Take t(r);
        REQUIRE(t.start(0, SyncTake::Record));
        t.hit(2, bar / 2, bar / 16);
        t.to(bar);
        REQUIRE(t.stop());
    }
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        const auto& s = r.engine->song();
        /* The first track kept its notes: a REC replaces only on the track it is on. */
        REQUIRE(s.tracks[0].clips.size() == 1);
        REQUIRE(s.tracks[0].clips[0].notes->notes.size() == 1);
        CHECK(s.tracks[0].clips[0].notes->notes[0].pad == 1);
        REQUIRE(s.tracks[1].clips.size() == 1);
        REQUIRE(s.tracks[1].clips[0].notes);
        REQUIRE(s.tracks[1].clips[0].notes->notes.size() == 1);
        CHECK(s.tracks[1].clips[0].notes->notes[0].pad == 2);
    }

    RoutedHits heard;
    r.seq.setSink(&RoutedHits::sink, &heard);
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    REQUIRE(r.engine->play(0));
    REQUIRE(waitFor([&] { r.block(out, bus); return r.engine->framesPlayed() > 0; }, 20000));
    while (r.engine->framesPlayed() < bar - bar / 8) r.block(out, bus);
    r.engine->stop();
    REQUIRE(waitFor([&] { return !r.engine->playing(); }));
    std::lock_guard<std::mutex> l(heard.m);
    CHECK(heard.ons == std::vector<std::pair<uint8_t, uint8_t>>{{0, 1}, {1, 2}});
}

TEST_CASE("song engine: Drums, Slice and Pitch tracks play at once, each on its own route", "[song][route][routes]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    const uint32_t bar = r.engine->framesPerBar();
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0, "/crosspad/kits/DRUMS/kit.json"));
        REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 1, "/crosspad/kits/SLICE/kit.json"));
        REQUIRE(s.addSeqTrack(2, SeqSource::Pitch, 2));
        s.tracks[2].seqBlocks = false;   // from before blocks had notes
        SeqNote d; d.pad = 1; d.vel = 100;
        SeqNote k; k.pad = 2; k.vel = 100; k.tick = AudioSong::kSeqPpq;
        s.putSeqTake(0, 0, makeSeqPattern(1, {d}), s.samplesPerBar(kRate));
        s.putSeqTake(1, 0, makeSeqPattern(1, {k}), s.samplesPerBar(kRate));
    }
    /* The Pitch track plays its scene over the whole song, as notes on its
     * own route: the clock's scene is the pads' kit's. */
    while (r.seq.sceneCount() < 3) REQUIRE(r.seq.addScene());
    r.seq.setSceneBars(2, 1);
    REQUIRE(r.seq.addEventTicks(2, 2 * AudioSong::kSeqPpq, 7, 100, 0, PadPerformance{}));
    r.engine->commit();

    RoutedHits heard;
    r.seq.setSink(&RoutedHits::sink, &heard);
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    REQUIRE(r.engine->play(0));
    REQUIRE(waitFor([&] { r.block(out, bus); return r.engine->framesPlayed() > 0; }, 20000));
    while (r.engine->framesPlayed() < bar - bar / 8) r.block(out, bus);
    r.engine->stop();
    REQUIRE(waitFor([&] { return !r.engine->playing(); }));
    std::lock_guard<std::mutex> l(heard.m);
    CHECK(heard.ons == std::vector<std::pair<uint8_t, uint8_t>>{{0, 1}, {1, 2}, {2, 7}});
}

TEST_CASE("song engine: a REC on one kit track is not heard under itself, the other plays on", "[song][songtake][route]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
        REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 2, "/crosspad/kits/B/kit.json"));   // its own scene
        s.armed = 1;
    }
    r.engine->commit();
    Take t(r);
    REQUIRE(t.start(0, SyncTake::Record));
    CHECK(r.seq.songRoutesMuted() == (1u << 1));
    REQUIRE(t.stop());
    CHECK(r.seq.songRoutesMuted() == 0);
}

TEST_CASE("song engine: playing with the seq view open runs the scene it edits", "[song][seqedit][route]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    REQUIRE(r.seq.addScene());                               // the second track's own, as Arrange makes it
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        s.length = 2;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
        REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 1, "/crosspad/kits/B/kit.json"));
    }
    r.engine->commit();
    /* The second kit's block in the seq view: a part recorded with the song
     * goes into its scene, not the first track's. */
    REQUIRE(r.engine->openSeqEdit(1, 0, 1));
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    REQUIRE(r.engine->play(0));
    REQUIRE(waitFor([&] { r.block(out, bus); return r.engine->framesPlayed() > 0; }, 20000));
    CHECK(r.seq.activeScene() == 1);
    r.engine->stop();
    REQUIRE(waitFor([&] { return !r.engine->playing(); }));
    r.engine->closeSeqEdit();
    REQUIRE(r.engine->play(0));
    REQUIRE(waitFor([&] { r.block(out, bus); return r.engine->framesPlayed() > 0; }, 20000));
    CHECK(r.seq.activeScene() == 0);
    r.engine->stop();
    REQUIRE(waitFor([&] { return !r.engine->playing(); }));
}

TEST_CASE("song engine: each kit track's fader is on its own engine", "[song][seqmix][route]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        s.length = 2;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
        REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 1, "/crosspad/kits/B/kit.json"));
        s.tracks[0].volume = 0.5f;
        s.tracks[1].volume = 0.25f;
    }
    r.engine->commit();
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    REQUIRE(r.engine->play(0));
    REQUIRE(waitFor([&] { r.block(out, bus); return r.engine->framesPlayed() > 0; }, 20000));
    CHECK(r.platform.kitGain[0] == 0.5f);
    CHECK(r.platform.kitGain[1] == 0.25f);
    CHECK(r.platform.seqGain[0] == 1.0f);                  // the old one-kit call is not made
    r.engine->stop();
    REQUIRE(waitFor([&] { return !r.engine->playing(); }));
    CHECK(r.platform.kitGain[0] == 1.0f);
    CHECK(r.platform.kitGain[1] == 1.0f);
}

TEST_CASE("song engine: the seq view of the second kit's block plays its scene on that kit's route", "[song][seqedit][route]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
        REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 1, "/crosspad/kits/B/kit.json"));
    }
    r.engine->commit();
    REQUIRE(r.engine->openSeqEdit(1, 0, 1));
    CHECK(r.seq.sceneRoute() == 1);
    r.engine->closeSeqEdit();
    CHECK(r.seq.sceneRoute() == 0);
    REQUIRE(r.engine->openSeqEdit(0, 0, 0));
    CHECK(r.seq.sceneRoute() == 0);
    r.engine->closeSeqEdit();
}

// ── the seq track through Arrange's mixer ─────────────────────────────────

TEST_CASE("song engine: a seq track's fader is on its engine while the song plays, and off at stop", "[song][seqmix]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        s.length = 2;
        REQUIRE(s.addSeqTrack(0, SeqSource::Pitch, 0));
        s.tracks[0].volume = 0.5f;
    }
    r.engine->commit();
    CHECK(r.platform.seqGain[1] == 1.0f);                  // stopped: the Mixer's level
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    REQUIRE(r.engine->play(0));
    REQUIRE(waitFor([&] { r.block(out, bus); return r.engine->framesPlayed() > 0; }, 20000));
    CHECK(r.platform.seqGain[1] == 0.5f);
    CHECK(r.platform.seqGain[0] == 1.0f);                  // the kit is not this track's
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[0].volume = 0.25f;         // a fader turned while it plays
    }
    r.engine->commit();
    CHECK(r.platform.seqGain[1] == 0.25f);
    r.engine->stop();
    REQUIRE(waitFor([&] { return !r.engine->playing(); }));
    CHECK(r.platform.seqGain[1] == 1.0f);
}

TEST_CASE("song engine: muting a seq track silences its notes as it plays, and unmuting brings them back", "[song][seqmix]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    struct Count { std::atomic<int> hits{0}; } heard;
    r.seq.setSink([](void* ctx, uint8_t, uint8_t, bool on) {
        if (on) ++static_cast<Count*>(ctx)->hits;
    }, &heard);
    r.seq.setSceneBars(0, 1);
    for (int b = 0; b < 4; ++b) REQUIRE(r.seq.addEventTicks(0, (uint32_t)b * AudioSong::kSeqPpq, 1, 100));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        s.length = 8;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
        s.tracks[0].seqBlocks = false;                    // its scene over the whole song
        s.tracks[0].muted = true;                          // muted before it starts
    }
    r.engine->commit();
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    REQUIRE(r.engine->play(0));
    REQUIRE(waitFor([&] { r.block(out, bus); return r.engine->framesPlayed() > 0; }, 20000));
    const uint32_t bar = r.engine->framesPerBar();
    while (r.engine->framesPlayed() < bar) r.block(out, bus);
    CHECK(heard.hits.load() == 0);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[0].muted = false;
    }
    r.engine->commit();
    REQUIRE(waitFor([&] {                                  // the worker hands the map over
        r.block(out, bus);
        return heard.hits.load() > 0;
    }, 20000));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[0].muted = true;
    }
    r.engine->commit();
    const int before = heard.hits.load();
    const uint32_t at = r.engine->framesPlayed();
    while (r.engine->framesPlayed() < at + bar) r.block(out, bus);
    CHECK(heard.hits.load() == before);
    r.engine->stop();
}


TEST_CASE("song engine: the platform hears every song that opens, with the kit it names", "[song][sounds]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().kit = "/crosspad/kits/X/kit.json";
    }
    r.engine->commit();
    REQUIRE(r.open("Other"));                              // new: no kit
    REQUIRE(waitFor([&] {                                  // the save of Demo lands
        std::FILE* f = std::fopen((r.dir / "SONGS" / "Demo" / "song.json").string().c_str(), "r");
        if (!f) return false;
        char buf[4096] = {};
        std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        return std::strstr(buf, "/crosspad/kits/X/kit.json") != nullptr;
    }));
    REQUIRE(r.open("Demo"));
    std::lock_guard<std::mutex> l(r.platform.openedMutex);
    REQUIRE(r.platform.openedKits.size() == 3);
    CHECK(r.platform.openedKits[0] == "Demo:");
    CHECK(r.platform.openedKits[1] == "Other:");
    CHECK(r.platform.openedKits[2] == "Demo:/crosspad/kits/X/kit.json");
}

// ── Arrange's REC onto an audio track ─────────────────────────────────────

namespace {

/// The sample tools as ArrangeAudioTake sees them: what was asked, and a
/// take that "records" by naming a WAV written beforehand.
struct FakeTools final : ISampleTools {
    RecInput    in = RecInput::Mic;
    bool        usb = false;
    std::vector<std::string> calls;
    RecStatus   st;
    std::string takeFile;
    uint32_t    takeFrames = 0;
    uint32_t    syncBar = 0;     // where a take with the song says it went in

    bool selectInput(RecInput i) override { in = i; calls.push_back("select " + std::to_string((int)i)); return true; }
    RecInput input() const override { return in; }
    bool inputAvailable(RecInput i) const override { return i != RecInput::Usb || usb; }
    void peaks(float& l, float& r) override { l = r = 0; }
    bool startRecording() override { calls.push_back("start"); st = RecStatus{}; st.recording = true; return true; }
    bool startRecordingWithSong(uint32_t bar) override { return startRecordingWithSong(bar, 0); }
    bool startRecordingWithSong(uint32_t bar, uint8_t beats) override {
        calls.push_back("start song " + std::to_string(bar) + " " + std::to_string(beats));
        st = RecStatus{};
        st.recording = true;
        st.withSong = true;
        st.fromBar = syncBar;
        return true;
    }
    void stopRecording() override {
        calls.push_back("stop");
        st.recording = false;
        st.file = takeFile;
        st.frames = takeFrames;
    }
    RecStatus recStatus() override { return st; }
    void releaseInput() override { calls.push_back("release"); }
    bool requestRecordings() override { return false; }
    std::vector<std::string> recordings() override { return {}; }
    bool requestOverview(const std::string&, uint32_t, uint32_t, uint16_t) override { return false; }
    WaveOverview overview() override { return {}; }
    bool requestSaveRegion(const std::string&, uint32_t, uint32_t) override { return false; }
    bool requestExportKit(const SliceKit&) override { return false; }
    ToolJob job() const override { return ToolJob::Idle; }
    std::string jobResult() override { return {}; }
    uint8_t jobProgress() const override { return 0; }
    bool preview(const std::string&, uint32_t, uint32_t, bool) override { return false; }
    void stopPreview() override {}
    bool previewing() const override { return false; }
    uint32_t previewFrame() const override { return 0; }
    void setHandoff(const std::string&, uint32_t, uint32_t) override {}
    bool takeHandoff(std::string&, uint32_t&, uint32_t&) override { return false; }
};

ArrangeAudioTake::Result landTake() {
    ArrangeAudioTake::Result res;
    waitFor([&] { res = arrangeAudioTake().poll(); return res.done; }, 10000);
    return res;
}

} // namespace

TEST_CASE("Arrange REC on an armed audio track records its input and lays the take on it", "[song][audiotake]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    placeClip(r, "beat.wav", 500, 0, 0, 4);           // a beat to sing over
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        REQUIRE(s.addAudioTrack(1));
        s.tracks[1].input = RecInput::MicLine;
        s.armed = 1;
    }
    r.engine->commit();
    FakeTools tools;
    tools.takeFile = (r.dir / "rec_001.wav").string();
    tools.takeFrames = kRate * 3;
    writeRamp(tools.takeFile, tools.takeFrames);
    tools.syncBar = 2;
    Services svc(r.engine.get(), &tools);

    SongTransport& tr = songTransport();
    tr.setStartBar(2);
    CHECK((tr.caps() & kTransportCanRecord) != 0);
    CHECK((tr.caps() & kTransportCanOverdub) == 0);    // nothing to dub onto audio
    tr.record();
    REQUIRE(arrangeAudioTake().recording());
    CHECK(arrangeAudioTake().track() == 1);
    CHECK(tr.state() == TransportState::Recording);
    CHECK(tools.calls == std::vector<std::string>{"select " + std::to_string((int)RecInput::MicLine), "start song 2 0"});
    tr.overdub();                                      // ignored
    CHECK(arrangeAudioTake().recording());

    tr.record();                                       // REC again ends it
    CHECK_FALSE(arrangeAudioTake().recording());
    const auto res = landTake();
    REQUIRE(res.done);
    CHECK(res.ok);
    CHECK(res.track == 1);
    CHECK(res.bar == 2);
    CHECK(tools.calls.back() == "release");
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const auto& cs = r.engine->song().tracks[1].clips;
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].file == "rec_001.wav");
    CHECK(cs[0].bar == 2);
    CHECK(cs[0].bars == 2);                            // 3 s at 2 s a bar
    CHECK(fs::exists(r.clipsDir("Demo") / "rec_001.wav"));
}

TEST_CASE("Arrange REC into an empty song records on its own from the playhead's bar", "[song][audiotake]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.bpm = 120.0f;
        REQUIRE(s.addAudioTrack(0));
        s.armed = 0;
    }
    r.engine->commit();
    FakeTools tools;
    tools.takeFile = (r.dir / "rec_002.wav").string();
    tools.takeFrames = kRate;
    writeRamp(tools.takeFile, tools.takeFrames);
    Services svc(r.engine.get(), &tools);

    SongTransport& tr = songTransport();
    tr.setStartBar(3);
    tr.record();
    REQUIRE(arrangeAudioTake().recording());
    CHECK(tools.calls.back() == "start");               // nothing to line up with
    tr.stop();                                          // STOP ends it too
    const auto res = landTake();
    REQUIRE(res.ok);
    CHECK(res.bar == 3);
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    REQUIRE(r.engine->song().tracks[0].clips.size() == 1);
    CHECK(r.engine->song().tracks[0].clips[0].bar == 3);
}

TEST_CASE("Arrange REC refuses an input that cannot record, and records nothing", "[song][audiotake]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        REQUIRE(s.addAudioTrack(0));
        s.tracks[0].input = RecInput::Usb;
        s.armed = 0;
    }
    FakeTools tools;
    Services svc(r.engine.get(), &tools);
    songTransport().record();
    CHECK_FALSE(arrangeAudioTake().recording());
    CHECK_FALSE(arrangeAudioTake().lastError().empty());
    CHECK(tools.calls.empty());
}

TEST_CASE("Arrange REC round a loop keeps every complete pass as a clip, the last on the track", "[song][audiotake]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    placeClip(r, "beat.wav", 500, 0, 0, 4);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.loop = true;
        s.loopStart = 1;
        s.loopEnd = 3;                                  // round bars 2 and 3
        REQUIRE(s.addAudioTrack(1));
        s.armed = 1;
    }
    r.engine->commit();
    const uint32_t bar = r.engine->framesPerBar();
    FakeTools tools;
    tools.takeFile = (r.dir / "rec_003.wav").string();
    /* In at bar 2 (0-based 1): a pass to the loop's end (2 bars), a second
     * whole loop, then half of a third, which is not kept. */
    tools.takeFrames = bar * 2 + bar * 2 + bar;
    tools.syncBar = 1;
    writeRamp(tools.takeFile, tools.takeFrames);
    Services svc(r.engine.get(), &tools);

    SongTransport& tr = songTransport();
    tr.setStartBar(1);
    tr.record();
    REQUIRE(arrangeAudioTake().recording());
    tr.record();
    const auto res = landTake();
    REQUIRE(res.ok);
    CHECK(res.passes == 2);
    CHECK(res.bar == 1);
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const auto& cs = r.engine->song().tracks[1].clips;
    REQUIRE(cs.size() == 1);                            // the last pass, over the first
    CHECK(cs[0].bar == 1);
    CHECK(cs[0].bars == 2);
    CHECK(cs[0].file == "rec_003_2.wav");
    /* Both passes are clips of their own, each one loop long, the second
     * starting where the first ended in the take. */
    const auto first = readWavData((r.clipsDir("Demo") / "rec_003.wav").string());
    const auto second = readWavData((r.clipsDir("Demo") / "rec_003_2.wav").string());
    REQUIRE(first.size() == (size_t)bar * 2 * 2);
    REQUIRE(second.size() == (size_t)bar * 2 * 2);
    CHECK(second.front() == int16_t((bar * 2) % 30000));
}

TEST_CASE("song engine: undo brings an element clip back whole", "[song][undo][element]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        SongClip c;
        c.element = ".elements/E00C0FFEE";
        c.file = "B0123456789ABCDEF.wav";
        c.bar = 2;
        c.bars = 4;
        c.from = 1;
        c.frames = 176400;
        r.engine->song().addClip(0, c);
    }
    r.engine->commit();
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[0].clips.clear();
    }
    r.engine->commit();
    REQUIRE(r.engine->undo());
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    REQUIRE(r.engine->song().tracks[0].clips.size() == 1);
    const SongClip& c = r.engine->song().tracks[0].clips[0];
    CHECK(c.element == ".elements/E00C0FFEE");
    CHECK(c.file == "B0123456789ABCDEF.wav");
    CHECK(c.from == 1);
    CHECK(c.frames == 176400);
}

namespace {
/// An element clip on track 0 of the open song: @p bake is its bake file
/// (empty: not baked yet), one pass of it @p passBars bars long at 120 BPM.
void placeElementClip(Rig& r, const std::string& bake, uint32_t passBars, uint32_t bar, uint32_t bars) {
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    r.engine->song().bpm = 120.0f;
    SongClip c;
    c.element = ".elements/E00000001";
    c.file = bake;
    c.bar = bar;
    c.bars = (uint16_t)bars;
    if (!bake.empty()) c.frames = passBars * r.engine->song().samplesPerBar(kRate);
    r.engine->song().addClip(0, c);
}
} // namespace

TEST_CASE("song engine: an element clip goes round its bake", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    const std::string bake = "B0000000000000001.wav";
    fs::create_directories(fs::path(r.platform.root) / AudioSong::bakeDir());
    const uint32_t spb = 2 * kRate;   // a bar at 120 BPM
    writeClip((fs::path(r.platform.root) / AudioSong::bakeDir() / bake).string(), 2 * spb, 16000);
    placeElementClip(r, bake, 2, 0, 4);
    r.engine->commit();
    CHECK(segmentsOf(*r.engine, 0) == 2);
    /* Bar 3 is in the second pass: silence there means it did not go round. */
    REQUIRE(r.engine->play(3));
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    REQUIRE(waitFor([&] {
        r.block(out, bus);
        return std::any_of(out.begin(), out.end(), [](float v) { return v != 0.0f; });
    }));
}

TEST_CASE("song engine: an element clip with no bake yet is silent", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    placeElementClip(r, "", 0, 0, 4);
    r.engine->commit();
    CHECK(segmentsOf(*r.engine, 0) == 0);
    REQUIRE(r.engine->play(0));
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    for (int i = 0; i < 50; ++i) {
        r.block(out, bus);
        for (float v : out) CHECK(v == 0.0f);
    }
    CHECK(r.engine->openFailures() == 0);
}

TEST_CASE("song engine: a bake deleted from the cache plays silence and is counted", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    placeElementClip(r, "B00000000000000FF.wav", 2, 0, 4);
    r.engine->commit();
    REQUIRE(r.engine->play(0));
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    REQUIRE(waitFor([&] {
        r.block(out, bus);
        return r.engine->openFailures() >= 1;
    }));
    for (float v : out) CHECK(v == 0.0f);
}

TEST_CASE("song engine: a new tempo leaves an element clip's bars alone", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    const std::string bake = "B0000000000000001.wav";
    fs::create_directories(fs::path(r.platform.root) / AudioSong::bakeDir());
    writeClip((fs::path(r.platform.root) / AudioSong::bakeDir() / bake).string(), 2 * 2 * kRate, 16000);
    /* The same name in clips/ too: a tempo change that measured the clip by
     * its file would find it there and resize the clip. */
    writeClip((r.clipsDir("Demo") / bake).string(), 2 * 2 * kRate, 16000);
    placeElementClip(r, bake, 2, 0, 6);
    placeClip(r, "c.wav", 1000, 1, 0, 2);   // an audio clip beside it, on track 1
    r.engine->commit();
    REQUIRE(r.engine->requestTempo(240.0f));   // a bar is 1 s: the audio clip's 4 s is four bars
    REQUIRE(editDone(r));
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    CHECK(r.engine->song().tracks[0].clips[0].bars == 6);
    CHECK(r.engine->song().tracks[1].clips[0].bars == 4);
}

// ── Elements: open, list, bake ────────────────────────────────────────────

namespace {

const char* const kElementRef = ".elements/E00000001";

/// An element project written straight onto the card, as a song that has
/// been through the engine would leave it.
void writeElement(Rig& r, const std::string& ref, const std::string& title,
                  const std::vector<std::string>& uses, uint32_t length) {
    AudioSong s;
    s.dir = r.platform.root + "/" + ref;
    fs::create_directories(fs::path(s.dir) / AudioSong::clipsDir());
    s.title = title;
    s.length = length;
    s.bpm = 120.0f;
    for (size_t i = 0; i < uses.size(); ++i) {
        s.tracks[i].inUse = true;
        SongClip c;
        c.element = uses[i];
        c.file = "B0000000000000001.wav";
        c.frames = 1000;
        c.bars = 1;
        s.addClip((int)i, c);
    }
    REQUIRE(s.save());
}

bool bakeAndWait(Rig& r) {
    return r.engine->requestBakeElement() &&
           waitFor([&] { return r.engine->editState() != SongOpState::Busy; }, 10000);
}

fs::path bakeFile(Rig& r) {
    return fs::path(r.platform.root) / AudioSong::bakeDir() / r.engine->bakeResult().file;
}

/// Two constant audio tracks, 2 bars at 120 bpm (88200 frames a bar).
void twoTracks(Rig& r) {
    writeClip((r.clipsDir("Song 1") / "a.wav").string(), kRate * 4, 8192);    // a quarter
    writeClip((r.clipsDir("Song 1") / "b.wav").string(), kRate * 4, 16384);   // a half
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.bpm = 120.0f;
        s.tracks[0].inUse = s.tracks[1].inUse = true;
        s.tracks[1].volume = 0.5f;
        s.addClip(0, {"a.wav", 0, 2});
        s.addClip(1, {"b.wav", 0, 2});
    }
    r.engine->commit();
}

} // namespace

TEST_CASE("song engine: an element opens without becoming the boot project", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Song 1"));
    CHECK_FALSE(r.engine->requestOpenElement("Song 2"));
    CHECK_FALSE(r.engine->requestOpenElement(".elements/../x"));
    REQUIRE(r.engine->requestOpenElement(kElementRef));
    REQUIRE(waitFor([&] { return r.engine->openState() == SongOpState::Ready; }));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        const std::string dir = r.engine->song().dir;
        const std::string tail = std::string("/") + kElementRef;
        REQUIRE(dir.size() > tail.size());
        CHECK(dir.compare(dir.size() - tail.size(), tail.size(), tail) == 0);
    }
    CHECK(r.platform.current == "Song 1");
    CHECK(fs::is_directory(fs::path(r.platform.root) / kElementRef / AudioSong::clipsDir()));
}

TEST_CASE("song engine: the projects list hides the library folders", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    fs::create_directories(fs::path(r.platform.root) / AudioSong::elementsDir());
    fs::create_directories(fs::path(r.platform.root) / AudioSong::bakeDir());
    REQUIRE(r.engine->requestProjects());
    REQUIRE(waitFor([&] { return !r.engine->projects().empty(); }));
    CHECK(r.engine->projects() == std::vector<std::string>{"Demo"});
}

TEST_CASE("song engine: the library lists elements with title, length and what they use", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Song 1"));
    writeElement(r, ".elements/E00000001", "Intro", {}, 4);
    writeElement(r, ".elements/E00000002", "Verse", {".elements/E00000001", ".elements/E00000001"}, 8);
    fs::create_directories(fs::path(r.platform.root) / AudioSong::elementsDir() / "not-an-element");
    const uint32_t gen = r.engine->listGeneration();
    REQUIRE(r.engine->requestElements());
    REQUIRE(waitFor([&] { return r.engine->listGeneration() != gen; }));
    const auto list = r.engine->elements();
    REQUIRE(list.size() == 2);
    CHECK(list[0].ref == ".elements/E00000001");
    CHECK(list[0].title == "Intro");
    CHECK(list[0].bars == 4);
    CHECK(list[0].uses.empty());
    CHECK(list[1].ref == ".elements/E00000002");
    CHECK(list[1].title == "Verse");
    CHECK(list[1].bars == 8);
    CHECK(list[1].uses == std::vector<std::string>{".elements/E00000001"});   // once, however many blocks
}

TEST_CASE("song engine: a bake mixes every heard track at its volume", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Song 1"));
    twoTracks(r);
    const SongOpState before = r.engine->editState();
    REQUIRE(bakeAndWait(r));
    INFO("report: " << (r.engine->editReport().empty() ? "" : r.engine->editReport()[0]) << " before=" << (int)before);
    REQUIRE(r.engine->editState() == SongOpState::Ready);
    const ElementBake bake = r.engine->bakeResult();
    CHECK_FALSE(bake.cached);
    CHECK(bake.bars == 2);
    CHECK(bake.frames == 2 * 2 * kRate);
    REQUIRE(fs::exists(bakeFile(r)));
    CHECK(bake.file.size() == 21);                          // B + 16 digits + .wav
    const auto pcm = readWavData(bakeFile(r).string());
    REQUIRE(pcm.size() == (size_t)bake.frames * 2);
    for (size_t i : {size_t(0), size_t(2 * 88200), pcm.size() - 2}) CHECK(std::abs(pcm[i] - 16384) <= 1);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        CHECK(r.engine->song().tracks[0].clips.size() == 1);   // the song is as it was
    }
    CHECK(r.platform.current == "Song 1");
}

TEST_CASE("song engine: a bake pans each track as the song plays it", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Song 1"));
    twoTracks(r);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[0].pan = -0.5f;   // the quarter: left whole, right halved
        r.engine->song().tracks[1].pan = 0.25f;   // the half at 0.5: right whole, left at three quarters
    }
    r.engine->commit();
    constexpr float kLeft = 8192 + 8192 * 0.75f, kRight = 8192 * 0.5f + 8192;
    constexpr float kFullScale = 32768.0f;   // the render's 1.0, a WAV sample's 32768
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    REQUIRE(r.engine->play(0));
    REQUIRE(waitFor([&] {
        r.block(out, bus);
        return out[0] != 0.0f;
    }));
    for (int i = 0; i < 20; ++i) r.block(out, bus);
    const float live[2] = {out[0] * kFullScale, out[1] * kFullScale};
    CHECK(std::abs(live[0] - kLeft) <= 1.0f);
    CHECK(std::abs(live[1] - kRight) <= 1.0f);
    r.engine->stop();
    REQUIRE(waitFor([&] { return !r.engine->playing(); }));

    REQUIRE(bakeAndWait(r));
    REQUIRE(r.engine->editState() == SongOpState::Ready);
    const auto pcm = readWavData(bakeFile(r).string());
    REQUIRE(pcm.size() == (size_t)r.engine->bakeResult().frames * 2);
    for (size_t i : {size_t(0), size_t(2 * 88200), pcm.size() - 2}) {
        CHECK(std::abs(pcm[i] - live[0]) <= 1.0f);
        CHECK(std::abs(pcm[i + 1] - live[1]) <= 1.0f);
    }
}

TEST_CASE("song engine: a silenced track is not in the bake", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Song 1"));
    twoTracks(r);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[1].muted = true;
    }
    r.engine->commit();
    REQUIRE(bakeAndWait(r));
    REQUIRE(r.engine->editState() == SongOpState::Ready);
    const auto pcm = readWavData(bakeFile(r).string());
    REQUIRE_FALSE(pcm.empty());
    CHECK(std::abs(pcm[0] - 8192) <= 1);
}

TEST_CASE("song engine: the same element baked again is found in the cache", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Song 1"));
    twoTracks(r);
    REQUIRE(bakeAndWait(r));
    REQUIRE(r.engine->editState() == SongOpState::Ready);
    const ElementBake first = r.engine->bakeResult();
    const auto written = fs::last_write_time(bakeFile(r));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(bakeAndWait(r));
    REQUIRE(r.engine->editState() == SongOpState::Ready);
    const ElementBake again = r.engine->bakeResult();
    CHECK(again.cached);
    CHECK(again.file == first.file);
    CHECK(again.frames == first.frames);
    CHECK(again.bars == first.bars);
    CHECK(fs::last_write_time(bakeFile(r)) == written);

    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[0].volume = 0.5f;
    }
    r.engine->commit();
    REQUIRE(bakeAndWait(r));
    CHECK_FALSE(r.engine->bakeResult().cached);             // another sound, another bake
    CHECK(r.engine->bakeResult().file != first.file);
}

TEST_CASE("song engine: a seq track not frozen refuses the bake", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Song 1"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        s.length = 4;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
        SeqNote n;
        n.pad = 1; n.vel = 100;
        s.putSeqTake(0, 0, makeSeqPattern(1, {n}), s.samplesPerBar(kRate));
    }
    r.engine->commit();
    REQUIRE(bakeAndWait(r));
    CHECK(r.engine->editState() == SongOpState::Failed);
    const auto report = r.engine->editReport();
    REQUIRE_FALSE(report.empty());
    CHECK(report[0].find("freeze") != std::string::npos);
    CHECK_FALSE(fs::exists(fs::path(r.platform.root) / AudioSong::bakeDir()));
}

TEST_CASE("song engine: an armed frozen seq track is baked from its freeze", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Song 1"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        s.length = 8;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
        SeqNote n;
        n.pad = 1; n.vel = 100;
        s.putSeqTake(0, 2, makeSeqPattern(1, {n}), s.samplesPerBar(kRate));
        s.armed = -1;
    }
    r.engine->commit();
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.25f);
    REQUIRE(r.engine->freezeTrack(0));
    for (int i = 0; i < 4000 && r.engine->bounceStatus().state == BounceState::Running; ++i) {
        r.block(out, bus);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    REQUIRE(r.engine->bounceStatus().state == BounceState::Done);
    const std::string freezeWav = (r.clipsDir("Song 1") / r.engine->bounceStatus().file).string();
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().armed = 0;              // armed, it plays live; the bake still has its freeze
    }
    r.engine->commit();
    REQUIRE(bakeAndWait(r));
    INFO("report: " << (r.engine->editReport().empty() ? "" : r.engine->editReport()[0]));
    REQUIRE(r.engine->editState() == SongOpState::Ready);
    const ElementBake bake = r.engine->bakeResult();
    CHECK(bake.bars == 8);
    const auto pcm = readWavData(bakeFile(r).string());
    const auto frozen = readWavData(freezeWav);
    REQUIRE_FALSE(frozen.empty());
    REQUIRE(pcm.size() == (size_t)bake.frames * 2);
    const size_t at = (size_t)2 * r.engine->framesPerBar() * 2;      // bar 3, where the block is
    CHECK(pcm[at + 2] == frozen[2]);
    CHECK(pcm[at + 2] != 0);
}

TEST_CASE("song engine: a bake the card cannot take fails and leaves no part file", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Song 1"));
    twoTracks(r);
    {
        std::FILE* f = std::fopen((fs::path(r.platform.root) / AudioSong::bakeDir()).string().c_str(), "wb");
        REQUIRE(f);
        std::fclose(f);                      // .bake is a file: nothing can be made in it
    }
    REQUIRE(bakeAndWait(r));
    CHECK(r.engine->editState() == SongOpState::Failed);
    CHECK_FALSE(r.engine->editReport().empty());
    CHECK(r.engine->bakeResult().file.empty());
}

TEST_CASE("song engine: an element is never renamed out of the library", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Song 1"));
    REQUIRE(r.engine->requestOpenElement(kElementRef));
    REQUIRE(waitFor([&] { return r.engine->openState() == SongOpState::Ready; }));
    REQUIRE(r.engine->requestRename("X"));
    REQUIRE(waitFor([&] { return r.engine->editState() != SongOpState::Busy; }));
    CHECK(r.engine->editState() == SongOpState::Failed);
    CHECK(fs::is_directory(fs::path(r.platform.root) / kElementRef));
    CHECK_FALSE(fs::exists(fs::path(r.platform.root) / "X"));
    CHECK(r.platform.current == "Song 1");
}

TEST_CASE("song engine: an open asked for right after a stop still runs", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    /* Both requests are set before the worker wakes: each round puts them as
     * close together as a caller can. */
    for (int round = 0; round < 50; ++round) {
        const bool element = round % 2 == 1;
        r.engine->stop();
        const bool asked = element ? r.engine->requestOpenElement(".elements/E00000001")
                                   : r.engine->requestOpen("Other");
        REQUIRE(asked);
        REQUIRE(waitFor([&] { return r.engine->openState() == SongOpState::Ready; }, 2000));
        r.engine->stop();
        REQUIRE(r.engine->requestOpen("Demo"));
        REQUIRE(waitFor([&] { return r.engine->openState() == SongOpState::Ready; }, 2000));
    }
}

TEST_CASE("song engine: a save due during playback does not keep the worker from waiting", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    writeClip((r.clipsDir("Demo") / "c.wav").string(), kRate * 8, 1000);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.bpm = 120.0f;
        s.addClip(0, {"c.wav", 0, 4});
    }
    r.engine->commit();
    REQUIRE(r.engine->play(0));
    REQUIRE(waitFor([&] { return r.engine->playing(); }));
    r.engine->commit();                                         // a save, due in kSaveDelayMs, that waits for the stop
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const int before = r.platform.waits;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    /* A worker that never waits makes no waitWake calls: it spins on the ring. */
    CHECK(r.platform.waits - before >= 10);
    r.engine->stop();
    REQUIRE(waitFor([&] { return !r.engine->playing(); }));
}

TEST_CASE("song engine: the elements' list waits for the stop while the song streams", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    placeClip(r, "c.wav", 1000, 0, 0, 4);
    r.engine->commit();
    writeElement(r, kElementRef, "Intro", {}, 4);
    REQUIRE(r.engine->play(0));
    REQUIRE(waitFor([&] { return r.engine->playing(); }));
    const uint32_t gen = r.engine->listGeneration();
    REQUIRE(r.engine->requestElements());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    /* Every element's song.json read under playback takes the worker off the
     * ring for longer than it lasts. */
    CHECK(r.engine->listGeneration() == gen);
    CHECK(r.engine->elements().empty());
    const int before = r.platform.waits;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(r.platform.waits - before >= 10);                  // it waits between fills, not spinning on the list
    r.engine->stop();
    REQUIRE(waitFor([&] { return r.engine->listGeneration() != gen; }));
    CHECK_FALSE(r.engine->playing());
    REQUIRE(r.engine->elements().size() == 1);
    CHECK(r.engine->elements()[0].title == "Intro");
}

TEST_CASE("song engine: an element pass shorter than any bake plays nothing", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    const std::string bake = "B0000000000000001.wav";
    fs::create_directories(fs::path(r.platform.root) / AudioSong::bakeDir());
    writeClip((fs::path(r.platform.root) / AudioSong::bakeDir() / bake).string(), kRate, 16000);
    placeElementClip(r, bake, 0, 0, 4);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[0].clips[0].frames = 1;     // a song.json cut short: a pass a frame
    }
    r.engine->commit();
    CHECK(segmentsOf(*r.engine, 0) == 0);
    {
        /* Half a bar: a bake made before the song slowed down still goes round. */
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[0].clips[0].frames = kRate;
    }
    r.engine->commit();
    CHECK(segmentsOf(*r.engine, 0) == 8);
}

TEST_CASE("song engine: a bake found in the cache does not show the last one's progress", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Song 1"));
    twoTracks(r);
    REQUIRE(bakeAndWait(r));
    REQUIRE(r.engine->editState() == SongOpState::Ready);
    REQUIRE(r.engine->editProgress() == 100);
    REQUIRE(r.engine->requestBakeElement());
    CHECK(r.engine->editProgress() == 0);
    REQUIRE(waitFor([&] { return r.engine->editState() != SongOpState::Busy; }, 10000));
    CHECK(r.engine->bakeResult().cached);
}

// ── an element made from a region, from a song ────────────────────────────

namespace {

bool madeAndWait(Rig& r, bool region, uint32_t from, uint32_t to, const std::string& name = {}) {
    if (!(region ? r.engine->requestElementFromRegion(from, to) : r.engine->requestElementFromSong(name))) return false;
    return waitFor([&] { return r.engine->editState() != SongOpState::Busy; }, 10000);
}

AudioSong elementOnCard(Rig& r, const std::string& ref) {
    AudioSong e;
    REQUIRE(e.load(r.platform.root + "/" + ref) == SongLoad::Loaded);
    return e;
}

size_t elementFolders(Rig& r) {
    const fs::path lib = fs::path(r.platform.root) / AudioSong::elementsDir();
    return fs::exists(lib) ? (size_t)std::distance(fs::directory_iterator(lib), fs::directory_iterator()) : 0;
}

/// "Demo": an audio track (a panned half-volume quarter, 4 bars) and a Kit seq
/// track with notes in its blocks, a kit, a song-kept instrument and a freeze.
void regionSong(Rig& r) {
    REQUIRE(r.open("Demo"));
    writeClip((r.clipsDir("Demo") / "a.wav").string(), kRate * 8, 8192);
    const fs::path pitch = fs::path(r.platform.root) / "Demo" / "pitch";
    fs::create_directories(pitch);
    writeClip((pitch / "root.wav").string(), 64, 100);
    std::ofstream((pitch / "instrument.json").string()) << "{}";
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.bpm = 120.0f;
        s.kit = "/crosspad/kits/x/kit.json";
        s.instrument = "pitch/instrument.json";
        s.tracks[0].inUse = true;
        s.tracks[0].volume = 0.5f;
        s.tracks[0].pan = -0.25f;
        s.tracks[0].stretch = StretchMode::KeepPitch;
        s.addClip(0, {"a.wav", 0, 4});
        REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 0));
        s.tracks[1].kit = "/crosspad/kits/y/kit.json";
        s.tracks[1].solo = true;
        SeqNote n;
        n.pad = 3; n.vel = 99; n.tick = AudioSong::kSeqPpq;
        s.putSeqTake(1, 0, makeSeqPattern(4, {n}), s.samplesPerBar(kRate));
        s.tracks[1].freeze.push_back({"x.wav", 0, 4});
        s.tracks[1].freezeKey = 7;
        s.armed = -1;
    }
    r.engine->commit();
}

} // namespace

TEST_CASE("song engine: an element from a region carries the tracks, the sounds and the recordings", "[song][element]") {
    Rig r;
    regionSong(r);
    REQUIRE(madeAndWait(r, true, 1, 3));
    INFO("report: " << (r.engine->editReport().empty() ? "" : r.engine->editReport()[0]));
    REQUIRE(r.engine->editState() == SongOpState::Ready);
    const std::string ref = r.engine->elementMade();
    REQUIRE(is_element_ref(ref));
    const AudioSong e = elementOnCard(r, ref);
    CHECK(e.length == 2);
    CHECK(e.bpm == 120.0f);
    CHECK(e.kit == "/crosspad/kits/x/kit.json");
    CHECK(e.instrument == "pitch/instrument.json");
    CHECK(fs::exists(fs::path(r.platform.root) / ref / "pitch" / "root.wav"));
    CHECK(fs::exists(fs::path(r.platform.root) / ref / "clips" / "a.wav"));
    const SongTrack& a = e.tracks[0];
    REQUIRE(a.clips.size() == 1);
    CHECK(a.inUse);
    CHECK(a.clips[0].bar == 0);
    CHECK(a.clips[0].bars == 2);
    CHECK(a.clips[0].offset == 88200);          // cut one bar in, at the song's 120 BPM
    CHECK(a.volume == 0.5f);
    CHECK(a.pan == -0.25f);
    CHECK(a.stretch == StretchMode::KeepPitch);
    const SongTrack& q = e.tracks[1];
    REQUIRE(q.clips.size() == 1);
    CHECK(q.kind == TrackKind::Seq);
    CHECK(q.source == SeqSource::Kit);
    CHECK(q.kit == "/crosspad/kits/y/kit.json");
    CHECK(q.solo);
    CHECK(q.seqBlocks);
    CHECK(q.clips[0].bars == 2);
    REQUIRE(q.clips[0].notes);
    CHECK(q.clips[0].notes->notes.size() == 1);
    CHECK(q.freeze.empty());
    CHECK(q.freezeKey == 0);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        const AudioSong& s = r.engine->song();
        CHECK(s.name == "Demo");
        CHECK(s.tracks[0].clips.size() == 1);
        CHECK(s.tracks[1].freeze.size() == 1);   // the song is as it was
    }
}

TEST_CASE("song engine: a seq track that plays its scene gives the element the scene's notes", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    REQUIRE(r.seq.addEventTicks(0, 0, 8, 100));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.bpm = 120.0f;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
        s.tracks[0].seqBlocks = false;              // a track from before blocks: it plays its scene
        s.length = 4;
    }
    r.engine->commit();
    REQUIRE(madeAndWait(r, true, 0, 2));
    REQUIRE(r.engine->editState() == SongOpState::Ready);
    const AudioSong e = elementOnCard(r, r.engine->elementMade());
    REQUIRE(e.tracks[0].clips.size() == 1);
    REQUIRE(e.tracks[0].clips[0].notes);
    CHECK(e.tracks[0].clips[0].notes->notes.size() == 1);
    CHECK(e.tracks[0].clips[0].notes->notes[0].pad == 8);
}

TEST_CASE("song engine: an element from a region that cannot be copied leaves nothing behind", "[song][element]") {
    Rig r;
    regionSong(r);
    fs::remove(r.clipsDir("Demo") / "a.wav");
    CHECK_FALSE(r.engine->requestElementFromRegion(3, 3));
    REQUIRE(madeAndWait(r, true, 1, 3));
    CHECK(r.engine->editState() == SongOpState::Failed);
    CHECK_FALSE(r.engine->editReport().empty());
    CHECK(r.engine->elementMade().empty());
    CHECK(elementFolders(r) == 0);
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    CHECK(r.engine->song().tracks[0].clips.size() == 1);
}

TEST_CASE("song engine: an element from a song copies it, clips and all, without its freezes", "[song][element]") {
    Rig r;
    {
        AudioSong s;
        s.dir = r.platform.root + "/Src";
        fs::create_directories(fs::path(s.dir) / "clips" / ".freeze");
        fs::create_directories(fs::path(s.dir) / "pitch");
        writeClip((fs::path(s.dir) / "clips" / "a.wav").string(), kRate * 4, 8192);
        writeClip((fs::path(s.dir) / "clips" / ".freeze" / "FRZ00000001.wav").string(), 64, 1);
        s.bpm = 90.0f;
        s.length = 2;
        s.tracks[0].inUse = true;
        s.addClip(0, {"a.wav", 0, 2});
        REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 0));
        s.tracks[1].freeze.push_back({".freeze/FRZ00000001.wav", 0, 2});
        s.tracks[1].freezeKey = 9;
        REQUIRE(s.save());
    }
    REQUIRE(r.open("Demo"));
    CHECK_FALSE(r.engine->requestElementFromSong("../Src"));
    REQUIRE(madeAndWait(r, false, 0, 0, "Nothing"));
    CHECK(r.engine->editState() == SongOpState::Failed);
    CHECK(elementFolders(r) == 0);
    REQUIRE(madeAndWait(r, false, 0, 0, "Src"));
    INFO("report: " << (r.engine->editReport().empty() ? "" : r.engine->editReport()[0]));
    REQUIRE(r.engine->editState() == SongOpState::Ready);
    const std::string ref = r.engine->elementMade();
    const AudioSong e = elementOnCard(r, ref);
    CHECK(e.title == "Src");
    CHECK(e.bpm == 90.0f);
    CHECK(e.tracks[0].clips.size() == 1);
    CHECK(e.tracks[1].freeze.empty());
    CHECK(e.tracks[1].freezeKey == 0);
    CHECK(fs::exists(fs::path(r.platform.root) / ref / "clips" / "a.wav"));
    CHECK_FALSE(fs::exists(fs::path(r.platform.root) / ref / "clips" / ".freeze"));
    CHECK(fs::exists(fs::path(r.platform.root) / "Src" / "clips" / ".freeze" / "FRZ00000001.wav"));
    CHECK(r.platform.current == "Demo");
}

TEST_CASE("song engine: an element from a region keeps each track's recording input", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    writeClip((r.clipsDir("Demo") / "a.wav").string(), kRate * 4, 8192);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.bpm = 120.0f;
        s.tracks[0].inUse = true;
        s.tracks[0].input = RecInput::Pads;
        s.addClip(0, {"a.wav", 0, 2});
    }
    r.engine->commit();
    REQUIRE(madeAndWait(r, true, 0, 2));
    REQUIRE(r.engine->editState() == SongOpState::Ready);
    CHECK(elementOnCard(r, r.engine->elementMade()).tracks[0].input == RecInput::Pads);
}

TEST_CASE("song engine: names that climb out of the song are refused, not copied", "[song][element]") {
    Rig r;
    regionSong(r);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[0].clips[0].file = "../Demo/clips/a.wav";
    }
    r.engine->commit();
    REQUIRE(madeAndWait(r, true, 1, 3));
    CHECK(r.engine->editState() == SongOpState::Failed);
    CHECK(r.engine->editReport().at(0).find("plain file name") != std::string::npos);
    CHECK(elementFolders(r) == 0);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[0].clips[0].file = "a.wav";
        r.engine->song().instrument = "../outside/instrument.json";
    }
    r.engine->commit();
    REQUIRE(madeAndWait(r, true, 1, 3));
    CHECK(r.engine->editState() == SongOpState::Failed);
    CHECK(r.engine->editReport().at(0).find("instrument") != std::string::npos);
    CHECK(elementFolders(r) == 0);
}

TEST_CASE("song engine: an element discarded leaves the card as it was, the open song stays", "[song][element]") {
    Rig r;
    regionSong(r);
    REQUIRE(madeAndWait(r, true, 1, 3));
    REQUIRE(r.engine->editState() == SongOpState::Ready);
    const std::string ref = r.engine->elementMade();
    CHECK_FALSE(r.engine->requestElementDiscard("Demo"));
    CHECK_FALSE(r.engine->requestElementDiscard(".elements/../Demo"));
    REQUIRE(r.engine->requestElementDiscard(ref));
    REQUIRE(waitFor([&] { return r.engine->editState() != SongOpState::Busy; }, 10000));
    CHECK(r.engine->editState() == SongOpState::Ready);
    CHECK_FALSE(fs::exists(fs::path(r.platform.root) / ref));
    CHECK(fs::exists(fs::path(r.platform.root) / "Demo"));
}

namespace {

/// SongSounds' platform side where nothing loads: only its step is asked for.
struct IdleSoundsHost final : ISongSoundsHost {
    std::string cardRoot() override { return {}; }
    bool kitsSettled() override { return true; }
    void loadKit(int) override {}
    void unloadKit() override {}
    bool instrumentOwnsEngine() override { return false; }
    const char* foregroundApp() override { return nullptr; }
    void warn(const char*) override {}
    void log(bool, const char*) override {}
};

/// The platform's song engine for one test, as SongSounds finds it.
struct SongEngineScope {
    ISongEngine* was;
    explicit SongEngineScope(ISongEngine* se) : was(getPlatformServices().songEngine) { getPlatformServices().songEngine = se; }
    ~SongEngineScope() { getPlatformServices().songEngine = was; }
};

} // namespace

TEST_CASE("song engine: a kit saved under its own name makes the freeze that plays it stale", "[song][freeze][kit]") {
    constexpr const char* kPlayed = "/crosspad/kits/FreezeKitA/kit.json";
    constexpr const char* kOther = "/crosspad/kits/FreezeKitB/kit.json";
    Rig r;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        s.length = 16;
        s.kit = kPlayed;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 0));
        SeqNote n;
        n.pad = 1; n.vel = 100;
        s.putSeqTake(0, 5, makeSeqPattern(1, {n}), s.samplesPerBar(kRate));
        s.armed = -1;
    }
    r.engine->commit();
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.25f);
    REQUIRE(r.engine->freezeTrack(0));
    for (int i = 0; i < 4000 && r.engine->bounceStatus().state == BounceState::Running; ++i) {
        r.block(out, bus);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    REQUIRE(r.engine->bounceStatus().state == BounceState::Done);
    REQUIRE(r.engine->freezeState(0) == FreezeState::Frozen);

    SongEngineScope scope(r.engine.get());
    IdleSoundsHost host;
    SongSounds sounds(host);
    sounds.step();   // what it has seen of the kits so far

    SECTION("another kit saved: still frozen") {
        kit_saved_notify(kOther);
        sounds.step();
        CHECK(r.engine->freezeState(0) == FreezeState::Frozen);
    }
    SECTION("saved right after the song opened, before any look at the kit: heard") {
        SongSounds opened(host);
        opened.songOpened();
        opened.step();
        kit_saved_notify(kPlayed);
        opened.step();
        CHECK(r.engine->freezeState(0) == FreezeState::Stale);
    }
    SECTION("the kit it plays saved: stale, and rendered again on request") {
        kit_saved_notify(kPlayed);
        sounds.step();
        CHECK(r.engine->freezeState(0) == FreezeState::Stale);
        sounds.step();   // one save is heard once
        CHECK(r.engine->freezeState(0) == FreezeState::Stale);
    }
}

TEST_CASE("song engine: a REC punched out while playing is heard on the next pass, without a stop", "[song][songtake]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    struct Count { std::atomic<int> pad3{0}; } heard;
    r.seq.setSink([](void* ctx, uint8_t pad, uint8_t, bool on) {
        if (on && pad == 3) ++static_cast<Count*>(ctx)->pad3;
    }, &heard);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        auto& s = r.engine->song();
        s.bpm = 120.0f;
        s.loop = true;
    }
    r.engine->commit();
    const uint32_t bar = r.engine->framesPerBar();
    Take t(r);
    REQUIRE(t.start(0, SyncTake::Record));
    for (uint32_t b = 0; b < 8; ++b) t.hit(3, b * bar / 4, bar / 16);   // two bars of quarters
    t.to(2 * bar + bar / 4);
    REQUIRE(r.engine->seqPunch(SyncTake::None));          // REC again: out, and playing on
    CHECK(r.engine->playing());
    /* The song comes back to its two bars: the sequencer with it, not past them. */
    REQUIRE(waitFor([&] { t.r.block(t.out, t.bus); return r.seq.songMapPosition() < 2 * bar; }, 20000));
    const int before = heard.pad3.load();
    t.to(r.engine->framesPlayed() + 4 * bar);              // round the two-bar song twice more
    CHECK(heard.pad3.load() - before >= 8);
    r.engine->stop();
    REQUIRE(waitFor([&] { return !r.engine->playing(); }));
}

TEST_CASE("Arrange's playhead is the song's: another song opened starts on its bar one", "[song][transport]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    Services svc(r.engine.get(), nullptr);
    SongTransport& tr = songTransport();
    tr.setStartBar(6);                                 // stopped on bar seven of this song
    CHECK(tr.startBar() == 6);
    CHECK(tr.position().bar == 7);
    REQUIRE(r.open("Fresh"));                          // NEW: an empty song
    CHECK(tr.startBar() == 0);
    CHECK(tr.position().bar == 1);
    tr.locate(2);                                      // the knob from there, not from bar seven
    CHECK(tr.startBar() == 2);
    REQUIRE(r.open("Fresh"));                          // a NEW after a delete: the same name
    CHECK(tr.startBar() == 0);
}

TEST_CASE("song engine: an element from a region keeps the song's scenes, so its kit tracks freeze", "[song][element]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    REQUIRE(r.seq.addScene());
    REQUIRE(r.seq.addScene());                             // scenes 0..2: the kit track's is 2
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.bpm = 120.0f;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 2));
        SeqNote n;
        n.pad = 3; n.vel = 99;
        s.putSeqTake(0, 0, makeSeqPattern(2, {n}), s.samplesPerBar(kRate));
        s.armed = -1;
    }
    r.engine->commit();
    REQUIRE(madeAndWait(r, true, 0, 2));
    REQUIRE(r.engine->editState() == SongOpState::Ready);
    const std::string ref = r.engine->elementMade();
    PatternSequencer scenes;
    REQUIRE(SequenceStore::load(scenes, r.platform.root + "/" + ref + "/" + SequenceStore::fileName()));
    CHECK(scenes.sceneCount() == 3);
    CHECK(elementOnCard(r, ref).tracks[0].scene == 2);
}

TEST_CASE("song engine: an element from a region keeps ARM on the track it was on", "[song][element]") {
    Rig r;
    regionSong(r);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().armed = 1;                        // the kit track: REC and DUB go there
    }
    r.engine->commit();
    REQUIRE(madeAndWait(r, true, 1, 3));
    REQUIRE(r.engine->editState() == SongOpState::Ready);
    CHECK(elementOnCard(r, r.engine->elementMade()).armed == 1);
}

TEST_CASE("song engine: a kit track whose blocks hold their notes freezes whatever its scene", "[song][freeze]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.bpm = 120.0f;
        REQUIRE(s.addSeqTrack(0, SeqSource::Kit, 5));      // a scene this sequencer has not got
        SeqNote n;
        n.pad = 3; n.vel = 99;
        s.putSeqTake(0, 0, makeSeqPattern(1, {n}), s.samplesPerBar(kRate));
        s.armed = -1;
    }
    r.engine->commit();
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.25f);
    REQUIRE(r.engine->freezeTrack(0));
    for (int i = 0; i < 6000 && r.engine->bounceStatus().state == BounceState::Running; ++i) {
        r.block(out, bus);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    INFO("error: " << r.engine->bounceStatus().error);
    CHECK(r.engine->bounceStatus().state == BounceState::Done);
    CHECK(r.engine->freezeState(0) == FreezeState::Frozen);
}

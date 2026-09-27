#include <catch2/catch_test_macros.hpp>

#include "crosspad/audio/WavWriter.hpp"
#include "crosspad/sequencer/MetronomeNode.hpp"
#include "crosspad/sequencer/PatternSequencer.hpp"
#include "crosspad/song/SongEngine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace crosspad;

namespace {

constexpr uint32_t kRate = 44100;
constexpr uint32_t kBlock = 256;

struct FakePlatform final : ISongEnginePlatform {
    std::string root;
    std::string current;
    std::thread worker;
    std::mutex m;
    std::condition_variable cv;
    bool woken = false;
    std::atomic<bool> storage{true};
    std::atomic<int> storageAsked{0};
    std::atomic<int> captureBegun{0}, captureEnded{0};

    bool startWorker(void (*entry)(void*), void* arg) override {
        worker = std::thread(entry, arg);
        return true;
    }
    void joinWorker() override { if (worker.joinable()) worker.join(); }
    void wake() override {
        { std::lock_guard<std::mutex> l(m); woken = true; }
        cv.notify_one();
    }
    void waitWake(uint32_t ms) override {
        std::unique_lock<std::mutex> l(m);
        if (ms == UINT32_MAX) cv.wait(l, [&] { return woken; });
        else cv.wait_for(l, std::chrono::milliseconds(ms), [&] { return woken; });
        woken = false;
    }
    void sleepMs(uint32_t ms) override { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
    int64_t nowUs() override {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void* allocRingSegment(size_t b, uint32_t) override { return std::malloc(b); }
    void freeRingSegment(void* p) override { std::free(p); }
    void* allocScratch(size_t b) override { return std::malloc(b); }
    size_t freeMemoryBytes() override { return size_t(1) << 30; }
    bool storageReady() override { ++storageAsked; return storage.load(); }
    void yieldIo() override {}
    const char* songsRoot() override { return root.c_str(); }
    std::string loadCurrent() override { return current; }
    void saveCurrent(const std::string& n) override { current = n; }
    void beginPadsCapture() override { ++captureBegun; }
    void endPadsCapture() override { ++captureEnded; }
    void log(bool, const char*) override {}
};

template <typename F>
bool waitFor(F cond, int ms = 3000) {
    for (int i = 0; i < ms; ++i) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return cond();
}

struct Rig {
    FakePlatform platform;
    MetronomeNode clock;
    PatternSequencer seq;
    std::unique_ptr<SongEngine> engine;
    fs::path dir;

    Rig() {
        dir = fs::temp_directory_path() / ("songengine_" + std::to_string(std::rand()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        platform.root = (dir / "SONGS").string();
        clock.onPrepare(kBlock, kRate);
        seq.attach(&clock);
        engine = std::make_unique<SongEngine>(platform, clock, seq);
        engine->onPrepare(kBlock, kRate);
    }
    ~Rig() { engine.reset(); fs::remove_all(dir); }

    /// One render block: clock first, then the song, then the capture tap.
    void block(std::vector<float>& out, const std::vector<float>& padsBus) {
        std::fill(out.begin(), out.end(), 0.0f);
        clock.mix(out.data(), kBlock);
        seq.poll();
        std::fill(out.begin(), out.end(), 0.0f);   // the click is not what is checked
        engine->mix(out.data(), kBlock);
        engine->rtCapture(padsBus.data(), kBlock);
    }

    bool open(const std::string& name) {
        return engine->requestOpen(name) &&
               waitFor([&] { return engine->openState() == SongOpState::Ready; });
    }

    fs::path clipsDir(const std::string& project) const {
        return fs::path(platform.root) / project / AudioSong::clipsDir();
    }
};

void writeClip(const std::string& path, uint32_t frames, int16_t value) {
    WavWriter w;
    REQUIRE(w.open(path.c_str(), kRate, 2));
    std::vector<int16_t> buf(frames * 2, value);
    REQUIRE(w.write(buf.data(), frames));
    REQUIRE(w.close());
}

/// The samples of a 16-bit WAV's data chunk.
std::vector<int16_t> readWavData(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    REQUIRE(f);
    std::vector<uint8_t> all;
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) all.insert(all.end(), buf, buf + n);
    std::fclose(f);
    size_t at = 12;
    while (at + 8 <= all.size()) {
        const uint32_t size = all[at + 4] | all[at + 5] << 8 | all[at + 6] << 16 | uint32_t(all[at + 7]) << 24;
        if (std::memcmp(&all[at], "data", 4) == 0) {
            std::vector<int16_t> pcm(size / 2);
            std::memcpy(pcm.data(), &all[at + 8], pcm.size() * 2);
            return pcm;
        }
        at += 8 + size + (size & 1);
    }
    return {};
}

} // namespace

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
    REQUIRE(r.engine->song().tracks[3].clips.size() == 1);
    CHECK(r.engine->song().tracks[3].clips[0].file == "take_2.wav");
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

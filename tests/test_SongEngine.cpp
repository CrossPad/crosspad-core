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
    bool storageReady() override { return storage.load(); }
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
    for (int i = 0; i < 20000 && r.engine->bounceStatus().state == BounceState::Running; ++i) {
        r.block(out, bus);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    const BounceStatus st = r.engine->bounceStatus();
    REQUIRE(st.state == BounceState::Done);
    CHECK(r.platform.captureBegun == 1);
    CHECK(r.platform.captureEnded == 1);
    const fs::path wav = r.clipsDir("Demo") / st.file;
    REQUIRE(fs::exists(wav));
    CHECK(fs::file_size(wav) > 44);
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    REQUIRE(r.engine->song().tracks[0].clips.size() == 1);
    CHECK(r.engine->song().tracks[0].clips[0].file == st.file);
}

TEST_CASE("song engine: a cancelled bounce leaves no file and restores the bus", "[song]") {
    Rig r;
    REQUIRE(r.open("Demo"));
    r.seq.setSceneBars(0, 4);
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.25f);
    REQUIRE(r.engine->bounceScene(0, 0));
    REQUIRE(waitFor([&] { return r.platform.captureBegun.load() == 1; }));
    for (int i = 0; i < 20; ++i) r.block(out, bus);
    r.engine->cancelBounce();
    REQUIRE(waitFor([&] { return r.engine->bounceStatus().state == BounceState::Failed; }));
    CHECK(r.engine->bounceStatus().error == "cancelled");
    CHECK(r.platform.captureEnded == r.platform.captureBegun);
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

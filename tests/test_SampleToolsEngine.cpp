// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "fake_file_system.hpp"

#include "crosspad/audio/WavWriter.hpp"
#include "crosspad/kit/PortableKitLoader.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sampletools/SampleToolsEngine.hpp"
#include "crosspad/song/ISongEngine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace crosspad;

namespace {

constexpr uint32_t kRate = 44100;
constexpr uint32_t kBlock = 256;

struct FakePlatform final : ISampleToolsPlatform {
    std::string root;
    std::thread worker;
    std::mutex m;
    std::condition_variable cv;
    bool woken = false;
    std::atomic<bool> storage{true};
    /// Ring segments are lent while this is on (and asked for).
    std::atomic<bool> lend{false};
    std::atomic<int> lentOut{0}, freed{0};
    std::vector<int> claims;          // +codec on a claim, -(codec + 1) on a release
    std::mutex claimMutex;

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
    void sleepMs(uint32_t ms) override { std::this_thread::sleep_for(std::chrono::milliseconds(ms ? ms : 1)); }
    int64_t nowUs() override {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void* allocRingSegment(size_t bytes, uint32_t, bool mayBorrow, bool& lent) override {
        lent = mayBorrow && lend.load();
        if (lent) ++lentOut;
        return std::malloc(bytes);
    }
    void freeRingSegment(void* p) override { ++freed; std::free(p); }
    void* allocScratch(size_t bytes) override { return std::malloc(bytes); }
    bool storageReady() override { return storage.load(); }
    void yieldIo() override {}
    std::string cardRoot() override { return root; }
    bool claimCodec(uint8_t codec, bool on) override {
        std::lock_guard<std::mutex> l(claimMutex);
        claims.push_back(on ? codec : -(codec + 1));
        return true;
    }
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

/// A stereo WAV whose frame i is (i, -i) as int16, wrapped.
void writeRamp(const std::string& path, uint32_t frames) {
    WavWriter w;
    REQUIRE(w.open(path.c_str(), kRate, 2));
    std::vector<int16_t> buf(frames * 2);
    for (uint32_t i = 0; i < frames; ++i) {
        buf[i * 2] = (int16_t)i;
        buf[i * 2 + 1] = (int16_t)-(int16_t)i;
    }
    REQUIRE(w.write(buf.data(), frames));
    REQUIRE(w.close());
}

/// A stereo WAV of @p frames at @p value.
void writeFlat(const std::string& path, uint32_t frames, int16_t value) {
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

/// A song engine that is already playing, at a position the test moves.
struct PlayingSong final : ISongEngine {
    AudioSong songModel;
    std::recursive_mutex mutex;
    std::atomic<uint32_t> position{0};
    std::atomic<int> plays{0}, stops{0};
    uint32_t spb = 1000;

    bool requestProjects() override { return false; }
    std::vector<std::string> projects() override { return {}; }
    bool requestOpen(const std::string&) override { return false; }
    SongOpState openState() const override { return SongOpState::Ready; }
    AudioSong& song() override { return songModel; }
    std::recursive_mutex& songMutex() override { return mutex; }
    void commit() override {}
    bool requestClips() override { return false; }
    std::vector<ClipInfo> clips() override { return {}; }
    uint32_t listGeneration() const override { return 0; }
    bool bounceScene(uint8_t, uint8_t) override { return false; }
    BounceStatus bounceStatus() override { return {}; }
    void cancelBounce() override {}
    uint32_t framesPerBar() override { return spb; }
    bool play(uint32_t) override { ++plays; return true; }
    void stop() override { ++stops; }
    bool playing() const override { return true; }
    double positionBars() const override { return (double)position.load() / spb; }
    uint32_t positionFrames() const override { return position.load(); }
    uint32_t underruns() const override { return 0; }
    uint32_t openFailures() const override { return 0; }
};

void touch(const fs::path& p) { std::FILE* f = std::fopen(p.string().c_str(), "wb"); REQUIRE(f); std::fclose(f); }

struct Rig {
    FakePlatform platform;
    std::unique_ptr<SampleToolsEngine> tools;
    fs::path dir;

    Rig() {
        dir = fs::temp_directory_path() /
              ("sampletools_" + std::to_string(std::random_device{}()) + "_" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        platform.root = dir.string();
        tools = std::make_unique<SampleToolsEngine>(platform);
        tools->onPrepare(kBlock, kRate);
    }
    ~Rig() { tools.reset(); fs::remove_all(dir); }

    fs::path recDir() const { return dir / "crosspad" / "recordings"; }

    /// Waits for the job asked for and says how it ended.
    ToolJob finished() {
        waitFor([&] { return tools->job() != ToolJob::Busy; }, 10000);
        return tools->job();
    }
};

} // namespace

TEST_CASE("SampleToolsEngine lists the recordings, newest name first", "[sampletools]") {
    Rig r;
    fs::create_directories(r.recDir());
    touch(r.recDir() / "rec_001.wav");
    touch(r.recDir() / "rec_002.WAV");
    touch(r.recDir() / "notes.txt");
    touch(r.recDir() / ".rec_003.wav");
    REQUIRE(r.tools->requestRecordings());
    REQUIRE(r.finished() == ToolJob::Ready);
    const auto recs = r.tools->recordings();
    REQUIRE(recs.size() == 2);
    CHECK(recs[0] == (r.recDir() / "rec_002.WAV").string());
    CHECK(recs[1] == (r.recDir() / "rec_001.wav").string());
}

TEST_CASE("SampleToolsEngine lists a folder: folders, then WAVs, by name", "[sampletools]") {
    Rig r;
    fs::create_directories(r.dir / "b_dir");
    fs::create_directories(r.dir / "A_dir");
    touch(r.dir / "z.wav");
    touch(r.dir / "B.wav");
    touch(r.dir / "x.txt");
    touch(r.dir / ".hidden.wav");

    SECTION("the card's top") {
        REQUIRE(r.tools->requestFolder(r.dir.string()));
        REQUIRE(r.finished() == ToolJob::Ready);
        const CardFolder f = r.tools->folder();
        CHECK(f.path == r.dir.string());
        REQUIRE(f.size() == 4);
        CHECK(f.folders == 2);
        CHECK(std::string(f.name(0)) == "A_dir");
        CHECK(std::string(f.name(1)) == "b_dir");
        CHECK(std::string(f.name(2)) == "B.wav");
        CHECK(std::string(f.name(3)) == "z.wav");
        CHECK_FALSE(f.truncated);
        CHECK(r.tools->jobResult() == r.dir.string());
    }
    SECTION("the recordings folder, not there yet: the card's top") {
        REQUIRE(r.tools->requestFolder(""));
        REQUIRE(r.finished() == ToolJob::Ready);
        CHECK(r.tools->folder().path == r.dir.string());
    }
    SECTION("never above the card") {
        REQUIRE(r.tools->requestFolder((r.dir / "A_dir" / ".." / "..").string()));
        REQUIRE(r.finished() == ToolJob::Ready);
        CHECK(r.tools->folder().path == r.dir.string());
        REQUIRE(r.tools->requestFolder("/somewhere/else"));
        REQUIRE(r.finished() == ToolJob::Ready);
        CHECK(r.tools->folder().path == r.dir.string());
    }
    SECTION("a trailing slash is dropped") {
        REQUIRE(r.tools->requestFolder((r.dir / "A_dir").string() + "/"));
        REQUIRE(r.finished() == ToolJob::Ready);
        CHECK(r.tools->folder().path == (r.dir / "A_dir").string());
        CHECK(r.tools->folder().size() == 0);
    }
    SECTION("no card") {
        r.platform.storage = false;
        REQUIRE(r.tools->requestFolder(r.dir.string()));
        REQUIRE(r.finished() == ToolJob::Failed);
        CHECK(r.tools->jobResult() == "no card");
    }
}

TEST_CASE("SampleToolsEngine stops reading a folder past kMaxEntries", "[sampletools][fs]") {
    test::FakeFileSystem card;
    test::UseFileSystem use(card);
    Rig r;
    const std::string root = r.dir.string();
    card.addDir(root);
    card.addDir(root + "/sub");
    for (size_t i = 0; i < CardFolder::kMaxEntries + 50; ++i) card.addFile(root + "/s" + std::to_string(i) + ".wav");
    REQUIRE(r.tools->requestFolder(root));
    REQUIRE(r.finished() == ToolJob::Ready);
    const CardFolder f = r.tools->folder();
    CHECK(f.truncated);
    CHECK(f.size() == CardFolder::kMaxEntries);
    CHECK(f.folders == 1);
    CHECK(std::string(f.name(0)) == "sub");
    CHECK(card.delivered == (int)CardFolder::kMaxEntries + 1);
}

TEST_CASE("SampleToolsEngine draws a file and its envelope", "[sampletools]") {
    Rig r;
    const std::string wav = (r.dir / "steps.wav").string();
    {
        WavWriter w;
        REQUIRE(w.open(wav.c_str(), kRate, 2));
        std::vector<int16_t> up(4096 * 2, 16000), down(4096 * 2, -16000);
        REQUIRE(w.write(up.data(), 4096));
        REQUIRE(w.write(down.data(), 4096));
        REQUIRE(w.close());
    }
    REQUIRE(r.tools->requestOverview(wav, 0, 0, 4));
    REQUIRE(r.finished() == ToolJob::Ready);
    CHECK(r.tools->jobResult() == wav);
    CHECK(r.tools->jobProgress() == 100);
    const WaveOverview ov = r.tools->overview();
    CHECK(ov.frames == 8192);
    CHECK(ov.start == 0);
    CHECK(ov.end == 8192);
    REQUIRE(ov.lo.size() == 4);
    CHECK(ov.hi[0] == 16000 / 258);
    CHECK(ov.lo[0] == 16000 / 258);
    CHECK(ov.lo[3] == -16000 / 258);
    CHECK(ov.hop == 256);
    CHECK(ov.envelope.size() == 8192 / 256);
    CHECK(ov.envelope.front() > 0);

    REQUIRE(r.tools->requestOverview(wav, 8192, 0, 4));
    REQUIRE(r.finished() == ToolJob::Failed);
    CHECK(r.tools->jobResult() == "empty range");

    REQUIRE(r.tools->requestOverview((r.dir / "none.wav").string(), 0, 0, 4));
    REQUIRE(r.finished() == ToolJob::Failed);
    CHECK(r.tools->jobResult() == "cannot read none.wav");
}

TEST_CASE("SampleToolsEngine saves a region into the recordings", "[sampletools]") {
    Rig r;
    fs::create_directories(r.dir / "kits");
    const std::string src = (r.dir / "kits" / "ramp.wav").string();
    writeRamp(src, 3000);

    REQUIRE(r.tools->requestSaveRegion(src, 100, 2600));
    REQUIRE(r.finished() == ToolJob::Ready);
    const std::string out = r.tools->jobResult();
    CHECK(out == (r.recDir() / "loop_001.wav").string());
    const auto pcm = readWavData(out);
    REQUIRE(pcm.size() == 2500 * 2);
    for (uint32_t i = 0; i < 2500; ++i) {
        REQUIRE(pcm[i * 2] == (int16_t)(100 + i));
        REQUIRE(pcm[i * 2 + 1] == (int16_t)-(int16_t)(100 + i));
    }

    REQUIRE(r.tools->requestSaveRegion(src, 0, 0));
    REQUIRE(r.finished() == ToolJob::Ready);
    CHECK(r.tools->jobResult() == (r.recDir() / "loop_002.wav").string());
    CHECK(readWavData(r.tools->jobResult()).size() == 3000 * 2);
}

TEST_CASE("SampleToolsEngine renders a pad's copy once", "[sampletools]") {
    Rig r;
    const std::string src = (r.dir / "ramp.wav").string();
    const std::string out = (r.dir / ".fx" / "ramp_r.wav").string();
    writeRamp(src, 2000);

    SampleFx none;
    CHECK_FALSE(r.tools->requestRendered(src, out, none));

    SampleFx rev;
    rev.reverse = true;
    REQUIRE(r.tools->requestRendered(src, out, rev));
    REQUIRE(r.finished() == ToolJob::Ready);
    CHECK(r.tools->jobResult() == out);
    CHECK_FALSE(fs::exists(out + ".tmp"));
    const auto pcm = readWavData(out);
    REQUIRE(pcm.size() == 2000 * 2);
    for (uint32_t i = 0; i < 2000; ++i) REQUIRE(pcm[i * 2] == (int16_t)(1999 - i));

    /* One already there is kept, whatever the source has become. */
    writeFlat(src, 10, 0);
    REQUIRE(r.tools->requestRendered(src, out, rev));
    REQUIRE(r.finished() == ToolJob::Ready);
    CHECK(readWavData(out).size() == 2000 * 2);
}

TEST_CASE("SampleToolsEngine writes slices out as a kit", "[sampletools]") {
    Rig r;
    PortableKitLoader loader(r.dir.string());
    loader.kits().reserve(PortableKitLoader::kSpareKits);
    IKitManager* before = getPlatformServices().kitManager;
    getPlatformServices().kitManager = &loader;

    fs::create_directories(r.dir / "crosspad");     // the kits' folder is made, not its parent
    const std::string src = (r.dir / "loop.wav").string();
    writeFlat(src, 4000, 20000);
    SliceKit kit;
    kit.source = src;
    kit.name = "Chops";
    kit.starts = {3000, 0, 1000};
    kit.end = 0;
    kit.bpm = 120;

    CHECK_FALSE(r.tools->requestExportKit(SliceKit{src, "bad/name", {0}, 0, 0, 0, 0}));
    REQUIRE(r.tools->requestExportKit(kit));
    const ToolJob exported = r.finished();
    INFO(r.tools->jobResult());
    REQUIRE(exported == ToolJob::Ready);
    CHECK(r.tools->jobResult() == "Chops");

    const fs::path kitDir = r.dir / "crosspad" / "kits" / "Chops";
    CHECK(readWavData((kitDir / "SAMPLES" / "1.wav").string()).size() == 1000 * 2);
    CHECK(readWavData((kitDir / "SAMPLES" / "2.wav").string()).size() == 2000 * 2);
    CHECK(readWavData((kitDir / "SAMPLES" / "3.wav").string()).size() == 1000 * 2);
    CHECK_FALSE(fs::exists(kitDir / "SAMPLES" / "4.wav"));
    REQUIRE(fs::exists(kitDir / "kit.json"));
    REQUIRE(loader.getKitCount() == 1);
    const KitInfo* listed = loader.getKit(0);
    CHECK(listed->name == "Chops");
    CHECK(listed->path == "/crosspad/kits/Chops/kit.json");
    CHECK(listed->bpm == 120);

    REQUIRE(r.tools->requestExportKit(kit));
    REQUIRE(r.finished() == ToolJob::Failed);
    CHECK(r.tools->jobResult() == "a kit called Chops is already on the card");

    getPlatformServices().kitManager = before;
}

TEST_CASE("SampleToolsEngine records the selected input to a take", "[sampletools]") {
    Rig r;
    REQUIRE(r.tools->selectInput(RecInput::Line));
    CHECK(r.platform.claims == std::vector<int>{1});
    CHECK(r.tools->wantsCodec(1));
    CHECK_FALSE(r.tools->wantsCodec(0));
    CHECK_FALSE(r.tools->inputAvailable(RecInput::Usb));

    REQUIRE(r.tools->startRecording());
    REQUIRE(waitFor([&] { return !r.tools->recStatus().file.empty(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));   // armed just after the file is named
    std::vector<int16_t> line(kBlock * 2, 1234), mics(kBlock * 2, -999);
    constexpr int kBlocks = 40;
    for (int b = 0; b < kBlocks; ++b) {
        r.tools->rtBlockBegin();
        r.tools->rtInput(0, mics.data(), kBlock);      // not the input selected
        r.tools->rtInput(1, line.data(), kBlock);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    float l = 0, rr = 0;
    r.tools->peaks(l, rr);
    CHECK(l == 1234.0f / 32768.0f);
    r.tools->stopRecording();
    REQUIRE(waitFor([&] { return !r.tools->recStatus().recording; }));
    const RecStatus st = r.tools->recStatus();
    CHECK(st.error.empty());
    CHECK(st.frames == kBlocks * kBlock);
    CHECK(st.file == (r.recDir() / "rec_001.wav").string());
    const auto pcm = readWavData(st.file);
    REQUIRE(pcm.size() == kBlocks * kBlock * 2);
    CHECK(std::all_of(pcm.begin(), pcm.end(), [](int16_t s) { return s == 1234; }));

    r.tools->releaseInput();
    CHECK(r.platform.claims == std::vector<int>{1, -2});
    CHECK_FALSE(r.tools->wantsCodec(1));
}

TEST_CASE("SampleToolsEngine records the pads bus, clipped to int16", "[sampletools]") {
    Rig r;
    REQUIRE(r.tools->selectInput(RecInput::Pads));
    CHECK(r.platform.claims.empty());
    REQUIRE(r.tools->startRecording());
    REQUIRE(waitFor([&] { return !r.tools->recStatus().file.empty(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::vector<float> bus(kBlock * 2, 2.0f);
    for (int b = 0; b < 4; ++b) r.tools->rtBus(bus.data(), kBlock);
    r.tools->stopRecording();
    REQUIRE(waitFor([&] { return !r.tools->recStatus().recording; }));
    const auto pcm = readWavData(r.tools->recStatus().file);
    REQUIRE(pcm.size() == 4 * kBlock * 2);
    CHECK(pcm.front() == 32767);
}

TEST_CASE("SampleToolsEngine leaves no file for a take that got nothing", "[sampletools]") {
    Rig r;
    REQUIRE(r.tools->selectInput(RecInput::Line));
    REQUIRE(r.tools->startRecording());
    REQUIRE(waitFor([&] { return !r.tools->recStatus().file.empty(); }));
    const std::string file = r.tools->recStatus().file;
    r.tools->stopRecording();                       // no block came in
    REQUIRE(waitFor([&] { return !r.tools->recStatus().recording; }));
    const RecStatus st = r.tools->recStatus();
    CHECK(st.error.empty());
    CHECK(st.frames == 0);
    CHECK_FALSE(fs::exists(file));
}

TEST_CASE("SampleToolsEngine fails a take with no card", "[sampletools]") {
    Rig r;
    r.platform.storage = false;
    REQUIRE(r.tools->selectInput(RecInput::Mic));
    REQUIRE(r.tools->startRecording());
    REQUIRE(waitFor([&] { return !r.tools->recStatus().recording; }));
    CHECK(r.tools->recStatus().error == "no card");
    CHECK_FALSE(r.tools->startRecordingWithSong(1));   // no song engine here
}

TEST_CASE("SampleToolsEngine auditions a region on its channel", "[sampletools]") {
    Rig r;
    const std::string src = (r.dir / "ramp.wav").string();
    writeRamp(src, 3000);
    REQUIRE(r.tools->preview(src, 1000, 2000, false));
    CHECK(r.tools->previewing());

    std::vector<float> out(kBlock * 2);
    std::vector<float> heard;
    REQUIRE(waitFor([&] {
        std::fill(out.begin(), out.end(), 0.0f);
        r.tools->mix(out.data(), kBlock);
        heard.insert(heard.end(), out.begin(), out.end());
        return !r.tools->previewing();
    }));
    /* The ring primes before anything is heard; what is heard is the region. */
    const auto first = std::find_if(heard.begin(), heard.end(), [](float s) { return s != 0.0f; });
    REQUIRE(first != heard.end());
    const size_t at = (size_t)(first - heard.begin());
    REQUIRE(heard.size() >= at + 1000 * 2);
    for (uint32_t i = 0; i < 1000; ++i) REQUIRE(heard[at + i * 2] == (float)(int16_t)(1000 + i) / 32768.0f);
}

TEST_CASE("SampleToolsEngine records on lent memory and ends the take when it is reclaimed", "[sampletools]") {
    Rig r;
    r.platform.lend = true;                         // a kit holds the platform's memory
    REQUIRE(r.tools->selectInput(RecInput::Line));
    REQUIRE(r.tools->startRecording());
    REQUIRE(waitFor([&] { return !r.tools->recStatus().file.empty(); }));
    REQUIRE(r.platform.lentOut.load() > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));   // armed just after the file is named
    std::vector<int16_t> line(kBlock * 2, 777);
    constexpr int kBlocks = 20;
    for (int b = 0; b < kBlocks; ++b) {
        r.tools->rtBlockBegin();
        r.tools->rtInput(1, line.data(), kBlock);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const int freedBefore = r.platform.freed.load();
    r.tools->reclaimLentRing();                     // another kit: the memory goes back
    REQUIRE(waitFor([&] { return !r.tools->recStatus().recording; }));
    CHECK(r.platform.freed.load() > freedBefore);
    const RecStatus st = r.tools->recStatus();
    CHECK(st.error.empty());
    CHECK(st.frames == kBlocks * kBlock);           // written out, ending at the reclaim
    REQUIRE(fs::exists(st.file));
    CHECK(readWavData(st.file).size() == kBlocks * kBlock * 2);
}

TEST_CASE("SampleToolsEngine gives a lent preview ring back", "[sampletools]") {
    Rig r;
    r.platform.lend = true;
    const std::string src = (r.dir / "long.wav").string();
    writeFlat(src, 20000, 100);
    REQUIRE(r.tools->preview(src, 0, 0, true));
    REQUIRE(waitFor([&] { return r.platform.lentOut.load() > 0; }));
    std::vector<float> out(kBlock * 2);
    r.tools->mix(out.data(), kBlock);
    const int freedBefore = r.platform.freed.load();
    r.tools->reclaimLentRing();
    CHECK_FALSE(r.tools->previewing());
    CHECK(r.platform.freed.load() > freedBefore);
}

TEST_CASE("SampleToolsEngine passes a handoff once", "[sampletools]") {
    Rig r;
    std::string f;
    uint32_t a = 0, b = 0;
    CHECK_FALSE(r.tools->takeHandoff(f, a, b));
    r.tools->setHandoff("/x.wav", 10, 20);
    REQUIRE(r.tools->takeHandoff(f, a, b));
    CHECK(f == "/x.wav");
    CHECK(a == 10);
    CHECK(b == 20);
    CHECK_FALSE(r.tools->takeHandoff(f, a, b));
}

TEST_CASE("SampleToolsEngine brings a sample up to a peak, as a copy", "[sampletools]") {
    Rig r;
    const std::string src = (r.dir / "quiet.wav").string();
    const std::string out = (r.dir / ".fx" / "quiet_n.wav").string();
    fs::create_directories(r.dir / ".fx");
    writeFlat(src, 3000, 8192);

    CHECK_FALSE(r.tools->requestNormalized(src, out, 0.0f));
    CHECK_FALSE(r.tools->requestNormalized(src, out, 1.5f));
    CHECK_FALSE(r.tools->requestNormalized("", out, 1.0f));

    REQUIRE(r.tools->requestNormalized(src, out, 1.0f));
    REQUIRE(r.finished() == ToolJob::Ready);
    CHECK(r.tools->jobResult() == out);
    CHECK_FALSE(fs::exists(out + ".tmp"));
    const auto pcm = readWavData(out);
    REQUIRE(pcm.size() == 3000 * 2);
    CHECK(std::all_of(pcm.begin(), pcm.end(), [](int16_t s) { return s >= 32766; }));

    /* One already there is kept. */
    writeFlat(src, 10, 100);
    REQUIRE(r.tools->requestNormalized(src, out, 1.0f));
    REQUIRE(r.finished() == ToolJob::Ready);
    CHECK(readWavData(out).size() == 3000 * 2);

    /* Never turned down: a file at the peak is its own answer, nothing written. */
    const std::string loud = (r.dir / "loud.wav").string();
    const std::string loudOut = (r.dir / ".fx" / "loud_n.wav").string();
    writeFlat(loud, 500, 30000);
    REQUIRE(r.tools->requestNormalized(loud, loudOut, 0.5f));
    REQUIRE(r.finished() == ToolJob::Ready);
    CHECK(r.tools->jobResult() == loud);
    CHECK_FALSE(fs::exists(loudOut));

    const std::string silent = (r.dir / "silent.wav").string();
    writeFlat(silent, 500, 0);
    REQUIRE(r.tools->requestNormalized(silent, (r.dir / ".fx" / "silent_n.wav").string(), 1.0f));
    REQUIRE(r.finished() == ToolJob::Failed);
    CHECK(r.tools->jobResult() == "the sample is silent");
}

TEST_CASE("SampleToolsEngine takes into a song already playing from its next bar line", "[sampletools][song]") {
    Rig r;
    PlayingSong song;
    ISongEngine* before = getPlatformServices().songEngine;
    getPlatformServices().songEngine = &song;

    REQUIRE(r.tools->selectInput(RecInput::Line));
    REQUIRE(r.tools->startRecordingWithSong(0));
    REQUIRE(waitFor([&] { return !r.tools->recStatus().file.empty(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));   // armed just after the file is named

    /* The song is 100 frames short of bar 1 when the first block begins, so
     * frame 0 of the take is input frame 100. */
    constexpr uint32_t kSongAtStart = 900;
    constexpr uint32_t kIntoBlock = 100;
    constexpr int kBlocks = 40;
    std::vector<int16_t> line(kBlock * 2);
    std::vector<float> bus(kBlock * 2, 0.0f);
    for (int b = 0; b < kBlocks; ++b) {
        for (uint32_t i = 0; i < kBlock; ++i) line[i * 2] = line[i * 2 + 1] = (int16_t)(b * kBlock + i);
        r.tools->rtBlockBegin();
        r.tools->rtInput(1, line.data(), kBlock);
        song.position = kSongAtStart + (uint32_t)(b + 1) * kBlock;
        r.tools->rtBus(bus.data(), kBlock);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    r.tools->stopRecording();
    REQUIRE(waitFor([&] { return !r.tools->recStatus().recording; }));
    const RecStatus st = r.tools->recStatus();
    CHECK(st.error.empty());
    CHECK(st.fromBar == 1);
    CHECK(song.plays == 0);                          // the song was left playing, not restarted
    const auto pcm = readWavData(st.file);
    REQUIRE(pcm.size() == (kBlocks * kBlock - kIntoBlock) * 2);
    CHECK(pcm[0] == (int16_t)kIntoBlock);
    CHECK(pcm[pcm.size() - 1] == (int16_t)(kBlocks * kBlock - 1));

    getPlatformServices().songEngine = before;
}

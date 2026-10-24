// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The song engine on a fake platform -- a directory for a card, a thread for its worker --
// and the helpers its tests share (SongEngine, ElementNav).

#include <catch2/catch_test_macros.hpp>

#include "fake_file_system.hpp"

#include "crosspad/audio/WavWriter.hpp"
#include "crosspad/sequencer/MetronomeNode.hpp"
#include "crosspad/sequencer/PatternSequencer.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sampletools/ISampleTools.hpp"
#include "crosspad/song/ArrangeAudioTake.hpp"
#include "crosspad/song/SongEngine.hpp"
#include "crosspad/transport/SongTransport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>
#include <zlib.h>

namespace fs = std::filesystem;
using namespace crosspad;

namespace {

constexpr uint32_t kRate = 44100;
constexpr uint32_t kBlock = 256;

std::string gunzip(const std::string& path) {
    std::string out;
    if (gzFile g = gzopen(path.c_str(), "rb")) {
        char buf[65536];
        int n = 0;
        while ((n = gzread(g, buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
        gzclose(g);
    }
    return out;
}

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
    /// writeIo the board's way: what stdio holds goes out first, then the
    /// piece straight to the descriptor. Off, the engine's default (fwrite).
    bool rawIo = false;
    /// The writeIo call, from 0, that fails; -1 for none.
    int failWriteAt = -1;
    /// Every writeIo: where in the file it started and how many bytes.
    std::mutex ioMutex;
    std::vector<std::pair<long, size_t>> writes;
    SongEngineSizes engineSizes;

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
    SongEngineSizes sizes() override { return engineSizes; }
    void* allocRingSegment(size_t b, uint32_t) override { return std::malloc(b); }
    void freeRingSegment(void* p) override { std::free(p); }
    void* allocScratch(size_t b) override { return std::malloc(b); }
    size_t freeMemoryBytes() override { return size_t(1) << 30; }
    bool storageReady() override { ++storageAsked; return storage.load(); }
    std::atomic<bool> holdIo{false};
    void yieldIo() override {
        while (holdIo.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool writeIo(FILE* f, const void* data, size_t bytes) override {
        std::lock_guard<std::mutex> l(ioMutex);
        if ((int)writes.size() == failWriteAt) return false;
        if (!rawIo) {
            writes.emplace_back(std::ftell(f), bytes);
            return ISongEnginePlatform::writeIo(f, data, bytes);
        }
        if (std::fflush(f) != 0) return false;
        const int fd = fileno(f);
        writes.emplace_back((long)::lseek(fd, 0, SEEK_CUR), bytes);
        return ::write(fd, data, bytes) == (ssize_t)bytes;
    }
    std::vector<std::pair<long, size_t>> writesSoFar() {
        std::lock_guard<std::mutex> l(ioMutex);
        return writes;
    }
    const char* songsRoot() override { return root.c_str(); }
    std::string loadCurrent() override { return current; }
    void saveCurrent(const std::string& n) override { current = n; }
    std::string liveSetTemplate() override { return gunzip(std::string(CROSSPAD_CORE_DIR) + "/data/live12_set.xml.gz"); }
    void beginPadsCapture() override { ++captureBegun; }
    std::atomic<int> routeCaptured{-1};
    void beginRouteCapture(uint8_t route) override { ++captureBegun; routeCaptured = route; }
    void endPadsCapture() override { ++captureEnded; }
    std::atomic<float> seqGain[2] = {{1.0f}, {1.0f}};   // kit, pitch
    std::mutex openedMutex;
    std::vector<std::string> openedKits;                 // AudioSong::kit at every songOpened()
    void songOpened(const AudioSong& s) override {
        std::lock_guard<std::mutex> l(openedMutex);
        openedKits.push_back(s.name + ":" + s.kit);
    }
    void setSeqGain(SeqSource s, float g) override { seqGain[s == SeqSource::Pitch ? 1 : 0] = g; }
    std::atomic<float> kitGain[2] = {{1.0f}, {1.0f}};   // by route
    void setKitGain(uint8_t route, float g) override { kitGain[route & 1] = g; }
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

    explicit Rig(SongEngineSizes sizes = {}) {
        /* ctest runs each case in its own process; an unseeded std::rand()
         * gave them all the same directory, so ctest -j removed each
         * other's clips. */
        dir = fs::temp_directory_path() /
              ("songengine_" + std::to_string(std::random_device{}()) + "_" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        platform.root = (dir / "SONGS").string();
        platform.engineSizes = sizes;
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

/// The segments flatten() gave track @p t, as debugLine() says them.
int segmentsOf(ISongEngine& e, int t) {
    const std::string line = e.debugLine();
    const size_t at = line.find("seg=");
    if (at == std::string::npos) return -1;
    int seg[4] = {-1, -1, -1, -1};
    std::sscanf(line.c_str() + at, "seg=%d/%d/%d/%d", &seg[0], &seg[1], &seg[2], &seg[3]);
    return seg[t];
}

/// The song engine and the tools as the board's services, put back after.
struct Services {
    ISongEngine* se;
    ISampleTools* st;
    Services(ISongEngine* e, ISampleTools* t) : se(getPlatformServices().songEngine), st(getPlatformServices().sampleTools) {
        getPlatformServices().songEngine = e;
        getPlatformServices().sampleTools = t;
    }
    ~Services() {
        getPlatformServices().songEngine = se;
        getPlatformServices().sampleTools = st;
    }
};

} // namespace

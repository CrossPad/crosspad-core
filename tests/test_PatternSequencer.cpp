// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "crosspad/sequencer/PatternSequencer.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace crosspad;

namespace {
constexpr uint32_t kRate  = 48000;
constexpr uint32_t kBlock = 256;
constexpr uint32_t kBeat  = 24000;          // 120 bpm at 48 kHz
constexpr uint32_t kTicksPerBeat = PatternSequencer::kPpq;

struct Rig {
    MetronomeNode clock;
    PatternSequencer seq;
    std::vector<uint8_t> fired;

    Rig() {
        clock.onPrepare(kBlock, kRate);
        clock.setBpm(120.0f);
        seq.attach(&clock);
        seq.setSink([](void* ctx, uint8_t pad, uint8_t, bool on) {
            if (on) static_cast<Rig*>(ctx)->fired.push_back(pad);
        }, this);
        seq.resetScenes();
    }
    /// Render @p frames through the clock in blocks, polling after each.
    void run(uint32_t frames) {
        std::vector<float> out(kBlock * 2);
        while (frames) {
            const uint32_t n = frames < kBlock ? frames : kBlock;
            clock.mix(out.data(), n);
            seq.poll();
            frames -= n;
        }
    }
    uint32_t loop(uint8_t bars) const { return kBeat * clock.beatsPerBar() * bars; }
};
} // namespace

TEST_CASE("sequencer: double plays the scene twice over", "[seq]") {
    Rig r;
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, 0, 1, 100));
    REQUIRE(r.seq.addEventTicks(0, kTicksPerBeat, 2, 90));

    REQUIRE(r.seq.doubleScene(0));
    REQUIRE(r.seq.sceneBars(0) == 2);
    REQUIRE(r.seq.eventCount(0) == 4);
    uint32_t pos = 0; uint8_t pad = 0, vel = 0;
    REQUIRE(r.seq.eventAt(0, 2, pos, pad, vel));
    REQUIRE(pos == r.loop(1));
    REQUIRE(pad == 1);
    REQUIRE(r.seq.eventAt(0, 3, pos, pad, vel));
    REQUIRE(pos == r.loop(1) + kBeat);
    REQUIRE(pad == 2);
}

TEST_CASE("sequencer: double refuses what would not fit", "[seq]") {
    Rig r;
    r.seq.setSceneBars(0, PatternSequencer::kMaxBars);
    REQUIRE_FALSE(r.seq.doubleScene(0));
    REQUIRE(r.seq.sceneBars(0) == PatternSequencer::kMaxBars);

    r.seq.setSceneBars(0, 1);
    const uint16_t half = PatternSequencer::kMaxEvents / 2 + 1;
    for (uint16_t i = 0; i < half; ++i) r.seq.addEventTicks(0, i, 0, 100);
    REQUIRE_FALSE(r.seq.doubleScene(0));
    REQUIRE(r.seq.eventCount(0) == half);
}

TEST_CASE("sequencer: halve keeps the first half", "[seq]") {
    Rig r;
    r.seq.setSceneBars(0, 2);
    REQUIRE(r.seq.addEventTicks(0, 0, 1, 100));
    REQUIRE(r.seq.addEventTicks(0, 4 * kTicksPerBeat, 2, 100));   // bar two
    REQUIRE(r.seq.halveScene(0));
    REQUIRE(r.seq.sceneBars(0) == 1);
    REQUIRE(r.seq.eventCount(0) == 1);
    REQUIRE_FALSE(r.seq.halveScene(0));                            // one bar is the floor
}

TEST_CASE("sequencer: a hit with erase held takes that pad out", "[seq]") {
    Rig r;
    r.seq.setSceneBars(0, 1);
    r.seq.addEventTicks(0, 0, 3, 100);
    r.seq.addEventTicks(0, kTicksPerBeat, 4, 100);
    r.seq.addEventTicks(0, 2 * kTicksPerBeat, 3, 100);
    r.seq.overdub();
    r.run(kBlock);
    r.seq.setErasing(true);
    r.seq.noteIn(3, 100);
    r.run(kBlock);
    r.seq.setErasing(false);
    REQUIRE(r.seq.eventCount(0) == 1);          // pad 3 gone, the hit not recorded
    uint32_t pos = 0; uint8_t pad = 0, vel = 0;
    REQUIRE(r.seq.eventAt(0, 0, pos, pad, vel));
    REQUIRE(pad == 4);
}

TEST_CASE("sequencer: sceneEvent places an event as a fraction of its scene", "[seq]") {
    Rig r;
    r.seq.setSceneBars(0, 1);
    r.seq.addEventTicks(0, 2 * kTicksPerBeat, 7, 80);
    float at = 0.0f; uint8_t pad = 0, vel = 0;
    REQUIRE(r.seq.sceneEvent(0, 0, at, pad, vel));
    REQUIRE(at == 0.5f);
    REQUIRE(pad == 7);
    REQUIRE(vel == 80);
    REQUIRE_FALSE(r.seq.sceneEvent(0, 1, at, pad, vel));
}

TEST_CASE("sequencer: scene edits racing the poll thread keep the pattern whole", "[seq][rt]") {
    // Double and halve from this thread while the poll thread plays, erases
    // and appends. Every event must stay inside its scene and the count
    // inside the array, whatever interleaving happened.
    Rig r;
    r.seq.setSceneBars(0, 1);
    for (uint8_t p = 0; p < 8; ++p) r.seq.addEventTicks(0, p * (kTicksPerBeat / 2), p, 100);
    r.seq.playback();
    std::atomic<bool> done{false};
    std::thread poller([&] {
        uint8_t pad = 0;
        while (!done.load(std::memory_order_relaxed)) {
            r.seq.setErasing((pad & 3) == 0);
            r.seq.noteIn(pad, 100);
            r.run(kBlock);
            pad = (uint8_t)((pad + 1) % 16);
        }
    });
    for (int i = 0; i < 2000; ++i) {
        if (i & 1) r.seq.halveScene(0);
        else       r.seq.doubleScene(0);
        for (uint8_t p = 0; p < 4; ++p) r.seq.addEventTicks(0, p * kTicksPerBeat, p, 100);
    }
    done.store(true);
    poller.join();

    const uint32_t loop = r.loop(r.seq.sceneBars(0));
    REQUIRE(r.seq.eventCount(0) <= PatternSequencer::kMaxEvents);
    for (uint16_t i = 0; i < r.seq.eventCount(0); ++i) {
        uint32_t pos = 0; uint8_t pad = 0, vel = 0;
        REQUIRE(r.seq.eventAt(0, i, pos, pad, vel));
        REQUIRE(pos < loop);
        REQUIRE(pad < 16);
    }
}

// ── Count-in ───────────────────────────────────────────────────────────────

namespace {
constexpr uint32_t kSixteenth = kBeat / 4;      // the default grid
constexpr uint32_t kMs = kRate / 1000;
}

TEST_CASE("sequencer: record from a stop counts a bar in first", "[seq][countin]") {
    Rig r;
    r.seq.setCountIn(true);
    r.seq.record();
    REQUIRE(r.seq.state() == SequencerState::PreBeat);
    REQUIRE(r.seq.countInLeft() == r.clock.beatsPerBar());
    r.run(kBeat * r.clock.beatsPerBar() + kBlock);
    REQUIRE(r.seq.state() == SequencerState::Running);
    REQUIRE(r.seq.countInLeft() == 0);
}

TEST_CASE("sequencer: count-in follows the metre", "[seq][countin]") {
    Rig r;
    r.clock.setBeatsPerBar(3);
    r.seq.setCountIn(true);
    r.seq.record();
    REQUIRE(r.seq.countInLeft() == 3);
}

TEST_CASE("sequencer: a two-bar count-in counts both bars, and is kept while off", "[seq][countin]") {
    Rig r;
    r.seq.setCountInBars(2);
    r.seq.setCountIn(true);
    r.seq.record();
    REQUIRE(r.seq.countInLeft() == 2 * r.clock.beatsPerBar());
    r.run(kBeat * r.clock.beatsPerBar() + kBlock);
    REQUIRE(r.seq.state() == SequencerState::PreBeat);
    r.run(kBeat * r.clock.beatsPerBar());
    REQUIRE(r.seq.state() == SequencerState::Running);
    r.seq.setCountIn(false);
    REQUIRE(r.seq.countInBeats() == 0);
    REQUIRE(r.seq.countInBars() == 2);
    r.seq.setCountInBars(9);
    REQUIRE(r.seq.countInBars() == PatternSequencer::kMaxCountInBars);
}

TEST_CASE("sequencer: without count-in a take starts at once", "[seq][countin]") {
    Rig r;
    r.seq.setCountIn(false);
    r.seq.record();
    REQUIRE(r.seq.state() == SequencerState::Running);
}

TEST_CASE("sequencer: a hit a hair before beat one lands on it", "[seq][countin]") {
    Rig r;
    r.seq.setCountIn(true);
    r.seq.record();
    r.run(kBeat * r.clock.beatsPerBar() - kMs);  // 1 ms before the downbeat
    r.seq.noteIn(3, 100);
    r.run(20 * kMs);                             // one slow poll later
    REQUIRE(r.seq.eventCount(0) == 1);
    uint32_t pos = 1; uint8_t pad = 0, vel = 0;
    REQUIRE(r.seq.eventAt(0, 0, pos, pad, vel));
    REQUIRE(pos == 0);
    REQUIRE(pad == 3);
}

TEST_CASE("sequencer: a hit early in the count-in is not recorded", "[seq][countin]") {
    Rig r;
    r.seq.setCountIn(true);
    r.seq.record();
    r.run(kBeat);                                // three beats still to count
    r.seq.noteIn(3, 100);
    r.run(kBeat * r.clock.beatsPerBar());
    REQUIRE(r.seq.eventCount(0) == 0);
}

TEST_CASE("sequencer: a hit just after beat one is kept where it fell", "[seq][countin]") {
    Rig r;
    r.seq.setCountIn(true);
    r.seq.record();
    r.run(kBeat * r.clock.beatsPerBar() + kSixteenth + kBlock);
    r.seq.noteIn(5, 100);
    r.run(kBlock);
    REQUIRE(r.seq.eventCount(0) == 1);
    uint32_t pos = 0; uint8_t pad = 0, vel = 0;
    REQUIRE(r.seq.eventAt(0, 0, pos, pad, vel));
    REQUIRE(pos == kSixteenth);
}

TEST_CASE("sequencer: the MIDI transport hears the start at beat one", "[seq][countin]") {
    Rig r;
    std::vector<int> seen;
    r.clock.setTransportHook([](bool running, void* ctx) {
        static_cast<std::vector<int>*>(ctx)->push_back(running ? 1 : 0);
    }, &seen);
    r.seq.setCountIn(true);
    r.seq.record();
    r.run(kBeat);
    REQUIRE(seen.empty());
    r.run(kBeat * r.clock.beatsPerBar());
    REQUIRE(seen == std::vector<int>{1});
}

TEST_CASE("sequencer: stop during the count-in goes idle", "[seq][countin]") {
    Rig r;
    r.seq.setCountIn(true);
    r.seq.record();
    r.run(kBeat);
    r.seq.stop();
    r.run(kBlock);
    REQUIRE(r.seq.state() == SequencerState::Idle);
    REQUIRE(r.seq.countInLeft() == 0);
}

TEST_CASE("sequencer: playback never counts in", "[seq][countin]") {
    Rig r;
    r.seq.setCountIn(true);
    r.seq.playback();
    REQUIRE(r.seq.state() == SequencerState::Running);
}

// ── Song ───────────────────────────────────────────────────────────────────

#include "crosspad/sequencer/SequenceStore.hpp"
#include <cstdio>

namespace {
/// Two one-bar scenes; the song [0 x2, 1 x1].
void twoSceneSong(Rig& r)
{
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addScene());
    r.seq.setSceneBars(1, 1);
    r.seq.select(0);
    REQUIRE(r.seq.songInsert(0, 0, 2));
    REQUIRE(r.seq.songInsert(1, 1, 1));
}
}

TEST_CASE("sequencer: song blocks are edited in place", "[seq][song]") {
    Rig r;
    twoSceneSong(r);
    uint8_t scene = 9, reps = 9;
    REQUIRE(r.seq.songLength() == 2);
    REQUIRE(r.seq.songBlock(1, scene, reps));
    REQUIRE(scene == 1);
    REQUIRE(reps == 1);
    REQUIRE(r.seq.songSet(1, 0, 200));
    REQUIRE(r.seq.songBlock(1, scene, reps));
    REQUIRE(reps == PatternSequencer::kSongMaxRepeats);
    REQUIRE_FALSE(r.seq.songInsert(5, 0, 1));     // past the end
    REQUIRE_FALSE(r.seq.songInsert(0, 7, 1));     // no such scene
    REQUIRE(r.seq.songRemove(0));
    REQUIRE(r.seq.songLength() == 1);
}

TEST_CASE("sequencer: a song plays its blocks in order and loops", "[seq][song]") {
    Rig r;
    twoSceneSong(r);
    r.seq.setSongMode(true);
    r.seq.playback();
    const uint32_t bar = r.loop(1);
    std::vector<uint8_t> scenes;
    for (int i = 0; i < 4; ++i) {
        scenes.push_back(r.seq.activeScene());
        r.run(bar);
    }
    REQUIRE(scenes == std::vector<uint8_t>{0, 0, 1, 0});
}

TEST_CASE("sequencer: a song without loop stops after its last block", "[seq][song]") {
    Rig r;
    twoSceneSong(r);
    r.seq.setSongLoop(false);
    r.seq.setSongMode(true);
    r.seq.playback();
    r.run(r.loop(1) * 3 + kBlock);
    REQUIRE(r.seq.state() == SequencerState::Idle);
}

TEST_CASE("sequencer: song mode after a stop starts at the first block", "[seq][song]") {
    Rig r;
    twoSceneSong(r);
    r.seq.setSongMode(true);
    r.seq.playback();
    r.run(r.loop(1) * 2 + kBlock);                // into block 1
    REQUIRE(r.seq.activeScene() == 1);
    r.seq.stop();
    REQUIRE_FALSE(r.seq.songMode());
    r.seq.setSongMode(true);
    r.seq.playback();
    REQUIRE(r.seq.songPosition() == 0);
    REQUIRE(r.seq.activeScene() == 0);
}

TEST_CASE("sequencer: locating a running song waits for the bar line", "[seq][song]") {
    Rig r;
    twoSceneSong(r);
    r.seq.setSongMode(true);
    r.seq.playback();
    r.run(kBlock);
    r.seq.songLocate(1);
    REQUIRE(r.seq.activeScene() == 0);
    r.run(r.loop(1));
    REQUIRE(r.seq.activeScene() == 1);
    REQUIRE(r.seq.songPosition() == 1);
}

TEST_CASE("sequencer: removing a scene takes its blocks with it", "[seq][song]") {
    Rig r;
    twoSceneSong(r);
    REQUIRE(r.seq.songInsert(2, 1, 3));
    REQUIRE(r.seq.removeScene(0));
    REQUIRE(r.seq.songLength() == 2);
    uint8_t scene = 9, reps = 0;
    REQUIRE(r.seq.songBlock(0, scene, reps));
    REQUIRE(scene == 0);                          // was scene 1
    REQUIRE(reps == 1);
}

TEST_CASE("sequencer: the arrangement travels in the sequence file", "[seq][song]") {
    const char* path = "cp_seq_song.json";
    Rig a;
    twoSceneSong(a);
    a.seq.setSongLoop(false);
    REQUIRE(SequenceStore::save(a.seq, path));

    Rig b;
    REQUIRE(SequenceStore::load(b.seq, path));
    std::remove(path);
    REQUIRE(b.seq.songLength() == 2);
    REQUIRE_FALSE(b.seq.songLoop());
    uint8_t scene = 9, reps = 0;
    REQUIRE(b.seq.songBlock(0, scene, reps));
    REQUIRE(scene == 0);
    REQUIRE(reps == 2);
}

TEST_CASE("sequencer: playbackAt starts part-way into the loop", "[seq][song]") {
    Rig r;
    r.seq.setSceneBars(0, 1);
    r.seq.addEventTicks(0, 3 * kTicksPerBeat, 9, 100);   // beat four
    r.seq.playbackAt(2 * kBeat);
    r.run(kBeat + kBlock);
    REQUIRE(r.fired == std::vector<uint8_t>{9});
}

TEST_CASE("sequencer: a slow pad callback does not hold the UI out of the scene", "[seq][rt]") {
    // The sink plays a pad; behind it the sample player may wait on the card.
    // A UI read of the pattern must not wait for that press to finish.
    using namespace std::chrono_literals;
    MetronomeNode clock;
    clock.onPrepare(kBlock, kRate);
    clock.setBpm(120.0f);
    PatternSequencer seq;
    seq.attach(&clock);
    seq.resetScenes();
    std::atomic<bool> inSink{false}, release{false};
    struct Ctx { std::atomic<bool>* in; std::atomic<bool>* go; } ctx{&inSink, &release};
    seq.setSink([](void* c, uint8_t, uint8_t, bool on) {
        auto* x = static_cast<Ctx*>(c);
        if (!on) return;
        x->in->store(true);
        while (!x->go->load()) std::this_thread::yield();
    }, &ctx);
    seq.setSceneBars(0, 1);
    seq.addEventTicks(0, 10, 2, 100);
    seq.playback();

    constexpr int kBlocksToEvent = 400;   // the event sits ~10 blocks in
    std::thread poller([&] {
        std::vector<float> out(kBlock * 2);
        for (int i = 0; i < kBlocksToEvent && !inSink.load(); ++i) {
            clock.mix(out.data(), kBlock);
            seq.poll();
        }
    });
    const auto t0 = std::chrono::steady_clock::now();
    while (!inSink.load() && std::chrono::steady_clock::now() - t0 < 5s) std::this_thread::yield();
    const bool sinkReached = inSink.load();

    std::atomic<bool> readDone{false};
    float at = 0.0f; uint8_t pad = 0, vel = 0;
    std::thread reader([&] { seq.sceneEvent(0, 0, at, pad, vel); readDone.store(true); });
    const auto t1 = std::chrono::steady_clock::now();
    while (!readDone.load() && std::chrono::steady_clock::now() - t1 < 200ms) std::this_thread::sleep_for(1ms);
    const bool readWhileSinkRuns = readDone.load();
    release.store(true);
    reader.join();
    poller.join();
    REQUIRE(sinkReached);
    REQUIRE(readWhileSinkRuns);
    REQUIRE(pad == 2);
}

// ── The scenes file ────────────────────────────────────────────────────────

namespace {
void writeText(const std::string& path, const char* text)
{
    std::FILE* f = std::fopen(path.c_str(), "w");
    REQUIRE(f != nullptr);
    std::fputs(text, f);
    std::fclose(f);
}

void removeAll(const std::string& path)
{
    for (const char* s : {"", ".tmp", ".bak"}) std::remove((path + s).c_str());
}
}

TEST_CASE("sequence file: no file at all is Absent", "[seq][store]") {
    const std::string path = "cp_seq_absent.json";
    removeAll(path);
    Rig r;
    REQUIRE(SequenceStore::open(r.seq, path) == SequenceLoad::Absent);
}

TEST_CASE("sequence file: a file that cannot be parsed is Unreadable, never Absent", "[seq][store]") {
    const std::string path = "cp_seq_bad.json";
    removeAll(path);
    writeText(path, "{\"version\":1,\"scenes\":[");
    Rig r;
    REQUIRE(SequenceStore::open(r.seq, path) == SequenceLoad::Unreadable);
    removeAll(path);
}

TEST_CASE("sequence file: a save cut short after the old file stepped aside still loads", "[seq][store]") {
    const std::string path = "cp_seq_cut.json";
    removeAll(path);
    Rig a;
    a.seq.setSceneBars(0, 1);
    a.seq.addEventTicks(0, 0, 3, 90);
    REQUIRE(SequenceStore::save(a.seq, path));
    /* What save() leaves if the board resets between its two renames. */
    REQUIRE(std::rename(path.c_str(), (path + ".tmp").c_str()) == 0);

    Rig b;
    REQUIRE(SequenceStore::open(b.seq, path) == SequenceLoad::Loaded);
    REQUIRE(b.seq.eventCount(0) == 1);
    removeAll(path);
}

TEST_CASE("sequence file: the previous save is the fallback for a torn one", "[seq][store]") {
    const std::string path = "cp_seq_torn.json";
    removeAll(path);
    Rig a;
    a.seq.setSceneBars(0, 1);
    a.seq.addEventTicks(0, 0, 3, 90);
    REQUIRE(SequenceStore::save(a.seq, path));
    REQUIRE(std::rename(path.c_str(), (path + ".bak").c_str()) == 0);
    writeText(path, "{\"version\":1,\"sce");

    Rig b;
    REQUIRE(SequenceStore::open(b.seq, path) == SequenceLoad::Loaded);
    REQUIRE(b.seq.eventCount(0) == 1);
    removeAll(path);
}

TEST_CASE("sequence file: a save leaves no .tmp or .bak behind", "[seq][store]") {
    const std::string path = "cp_seq_clean.json";
    removeAll(path);
    Rig a;
    REQUIRE(SequenceStore::save(a.seq, path));
    REQUIRE(SequenceStore::save(a.seq, path));
    REQUIRE(std::fopen((path + ".tmp").c_str(), "r") == nullptr);
    REQUIRE(std::fopen((path + ".bak").c_str(), "r") == nullptr);
    removeAll(path);
}

TEST_CASE("sequencer: stop and play racing the poll thread release every pad", "[seq][rt]") {
    // The transport is driven from the UI, the CDC loop and the song worker
    // while the poll thread fires the pattern. Every press the sink saw must
    // be matched by a release once the transport is down.
    Rig r;
    std::array<std::atomic<int>, 16> held{};
    r.seq.setSink([](void* ctx, uint8_t pad, uint8_t, bool on) {
        auto& h = *static_cast<std::array<std::atomic<int>, 16>*>(ctx);
        if (pad < 16) h[pad].fetch_add(on ? 1 : -1, std::memory_order_relaxed);
    }, &held);
    r.seq.setSceneBars(0, 1);
    for (uint8_t p = 0; p < 16; ++p) r.seq.addEventTicks(0, p * (kTicksPerBeat / 4), p, 100);
    r.seq.playback();
    std::atomic<bool> done{false};
    std::thread poller([&] {
        while (!done.load(std::memory_order_relaxed)) r.run(kBlock);
    });
    for (int i = 0; i < 2000; ++i) {
        r.seq.stop();
        r.seq.playback();
    }
    done.store(true);
    poller.join();
    r.seq.stop();
    for (uint8_t p = 0; p < 16; ++p) REQUIRE(held[p].load() == 0);
}

TEST_CASE("sequence file: a file that is there but will not open is Unreadable", "[seq][store]") {
    // On the board that is every FAT handle taken at boot: the file is there,
    // and reading it as "no scenes" lets the next save write over it.
    if (geteuid() == 0) SKIP("root opens anything");
    const std::string path = "cp_seq_locked.json";
    removeAll(path);
    Rig a;
    REQUIRE(SequenceStore::save(a.seq, path));
    REQUIRE(chmod(path.c_str(), 0) == 0);
    Rig b;
    const SequenceLoad got = SequenceStore::open(b.seq, path);
    chmod(path.c_str(), 0644);
    removeAll(path);
    REQUIRE(got == SequenceLoad::Unreadable);
}

TEST_CASE("sequence file: open replaces every scene, and leaves them if it cannot read", "[seq][store]") {
    const std::string path = "cp_seq_replace.json";
    removeAll(path);
    Rig one;
    one.seq.setSceneBars(0, 1);
    one.seq.addEventTicks(0, 0, 1, 90);
    REQUIRE(SequenceStore::save(one.seq, path));

    Rig r;
    REQUIRE(r.seq.addScene());
    REQUIRE(r.seq.addScene());
    r.seq.setSceneBars(2, 1);
    r.seq.addEventTicks(2, 0, 5, 90);
    r.seq.select(2);
    REQUIRE(r.seq.songInsert(0, 2, 1));
    r.seq.setSongMode(true);
    REQUIRE(SequenceStore::open(r.seq, path) == SequenceLoad::Loaded);
    REQUIRE_FALSE(r.seq.songMode());              // nothing of the old scenes survives
    REQUIRE(r.seq.sceneCount() == 1);
    REQUIRE(r.seq.eventCount(0) == 1);
    REQUIRE(r.seq.eventCount(2) == 0);

    writeText(path, "{\"version\":1,\"sce");
    r.seq.addEventTicks(0, kTicksPerBeat, 2, 90);
    REQUIRE(SequenceStore::open(r.seq, path) == SequenceLoad::Unreadable);
    REQUIRE(r.seq.eventCount(0) == 2);
    removeAll(path);
}

// ── Note length ────────────────────────────────────────────────────────────

namespace {
/// Every press and release the sink saw, with the clock position it came at.
struct Edge { uint8_t pad; bool on; uint32_t at; };
struct EdgeRig : Rig {
    std::vector<Edge> edges;
    EdgeRig() {
        seq.setSink([](void* ctx, uint8_t pad, uint8_t, bool on) {
            auto* r = static_cast<EdgeRig*>(ctx);
            r->edges.push_back({ pad, on, r->clock.positionSamples() });
        }, this);
    }
    const Edge* first(uint8_t pad, bool on) const {
        for (const Edge& e : edges) if (e.pad == pad && e.on == on) return &e;
        return nullptr;
    }
};
}

TEST_CASE("sequencer: a held pad records how long it was held", "[seq][len]") {
    Rig r;
    r.seq.setCountIn(false);
    r.seq.record();
    r.run(kBlock);
    r.seq.noteIn(3, 100);
    r.run(kBeat);
    r.seq.noteRelease(3);
    r.run(kBlock);
    REQUIRE(r.seq.eventCount(0) == 1);
    REQUIRE(r.seq.eventLength(0, 0) == kBeat);
}

TEST_CASE("sequencer: a release outside a take records nothing", "[seq][len]") {
    Rig r;
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, 0, 3, 100));
    r.seq.playback();
    r.run(kBlock);
    r.seq.noteIn(3, 100);
    r.run(kBeat);
    r.seq.noteRelease(3);
    r.run(kBlock);
    REQUIRE(r.seq.eventCount(0) == 1);
    REQUIRE(r.seq.eventLength(0, 0) == 0);
}

TEST_CASE("sequencer: a hold over the loop end is measured across it", "[seq][len]") {
    Rig r;
    r.seq.setCountIn(false);
    r.seq.setSceneBars(0, 1);
    r.seq.record();
    r.run(3 * kBeat);                          // beat four
    r.seq.noteIn(6, 100);
    r.run(2 * kBeat);                          // to beat two of the next pass
    r.seq.noteRelease(6);
    r.run(kBlock);
    REQUIRE(r.seq.eventCount(0) == 1);
    REQUIRE(r.seq.eventLength(0, 0) == 2 * kBeat);
}

TEST_CASE("sequencer: a pad still held when the take stops ends there", "[seq][len]") {
    Rig r;
    r.seq.setCountIn(false);
    r.seq.record();
    r.run(kBlock);
    r.seq.noteIn(2, 100);
    r.run(kBeat);
    r.seq.stop();
    r.seq.noteRelease(2);
    r.run(kBlock);
    REQUIRE(r.seq.eventLength(0, 0) >= kBeat);
    REQUIRE(r.seq.eventLength(0, 0) <= kBeat + kBlock);
}

TEST_CASE("sequencer: playback holds a note for its length", "[seq][len]") {
    EdgeRig r;
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, kTicksPerBeat, 4, 100, 2 * kTicksPerBeat));
    r.seq.playback();
    r.run(r.loop(1) - kBlock);
    const Edge* on  = r.first(4, true);
    const Edge* off = r.first(4, false);
    REQUIRE(on);
    REQUIRE(off);
    REQUIRE(off->at - on->at >= 2 * kBeat);
    REQUIRE(off->at - on->at <= 2 * kBeat + kBlock);
}

TEST_CASE("sequencer: a note longer than the rest of the loop releases on the next pass", "[seq][len]") {
    EdgeRig r;
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, 3 * kTicksPerBeat, 5, 100, 2 * kTicksPerBeat));
    r.seq.playback();
    r.run(r.loop(1) + kBeat + 2 * kBlock);
    const Edge* off = r.first(5, false);
    REQUIRE(off);
    REQUIRE(off->at >= kBeat);
    REQUIRE(off->at <= kBeat + kBlock);
}

TEST_CASE("sequencer: a tap with no length is released on the next poll", "[seq][len]") {
    EdgeRig r;
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, kTicksPerBeat, 4, 100));
    r.seq.playback();
    r.run(2 * kBeat);
    const Edge* on  = r.first(4, true);
    const Edge* off = r.first(4, false);
    REQUIRE(on);
    REQUIRE(off);
    REQUIRE(off->at - on->at <= kBlock);
}

TEST_CASE("sequencer: stop releases a note held by its length", "[seq][len]") {
    EdgeRig r;
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, 0, 7, 100, 3 * kTicksPerBeat));
    r.seq.playback();
    r.run(kBeat);
    REQUIRE(r.first(7, true));
    REQUIRE_FALSE(r.first(7, false));
    r.seq.stop();
    REQUIRE(r.first(7, false));
}

TEST_CASE("sequencer: a tempo change keeps a held note's length in beats", "[seq][len]") {
    Rig r;
    REQUIRE(r.seq.addEventTicks(0, 0, 1, 100, kTicksPerBeat));
    r.seq.setClickBpm(60.0f);
    REQUIRE(r.seq.eventLength(0, 0) == 2 * kBeat);
    REQUIRE(r.seq.eventLengthTicks(0, 0) == kTicksPerBeat);
}

TEST_CASE("sequencer: double copies note lengths", "[seq][len]") {
    Rig r;
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, 0, 1, 100, kTicksPerBeat / 2));
    REQUIRE(r.seq.doubleScene(0));
    REQUIRE(r.seq.eventLength(0, 1) == kBeat / 2);
}

TEST_CASE("sequencer: sceneEventLength is a fraction of the scene", "[seq][len]") {
    Rig r;
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, 0, 1, 100, kTicksPerBeat));
    REQUIRE(r.seq.sceneEventLength(0, 0) == 0.25f);
}

TEST_CASE("sequence file: note lengths travel, and a file without them loads as taps", "[seq][len][store]") {
    const std::string path = "cp_seq_len.json";
    removeAll(path);
    Rig a;
    REQUIRE(a.seq.addEventTicks(0, 0, 1, 100, kTicksPerBeat));
    REQUIRE(a.seq.addEventTicks(0, kTicksPerBeat, 2, 100));
    REQUIRE(SequenceStore::save(a.seq, path));
    Rig b;
    REQUIRE(SequenceStore::load(b.seq, path));
    REQUIRE(b.seq.eventLengthTicks(0, 0) == kTicksPerBeat);
    REQUIRE(b.seq.eventLengthTicks(0, 1) == 0);

    writeText(path, "{\"version\":1,\"ppq\":96,\"bpm\":120,\"scenes\":[{\"bars\":1,\"events\":[[0,3,90]]}]}");
    Rig c;
    REQUIRE(SequenceStore::load(c.seq, path));
    REQUIRE(c.seq.eventCount(0) == 1);
    REQUIRE(c.seq.eventLength(0, 0) == 0);
    removeAll(path);
}

TEST_CASE("sequencer: a pad still down is drawn as long as it has been held", "[seq][len]") {
    Rig r;
    r.seq.setCountIn(false);
    r.seq.setSceneBars(0, 1);
    r.seq.record();
    r.run(kBlock);
    r.seq.noteIn(3, 100);
    r.run(kBeat);
    REQUIRE(r.seq.eventLength(0, 0) == 0);                       // not known yet
    REQUIRE(r.seq.sceneEventLength(0, 0) == (float)kBeat / (float)r.loop(1));
    r.run(kBeat);
    REQUIRE(r.seq.sceneEventLength(0, 0) == (float)(2 * kBeat) / (float)r.loop(1));
}

TEST_CASE("sequencer: a pad held longer than the loop is a note the whole loop long", "[seq][len]") {
    Rig r;
    r.seq.setCountIn(false);
    r.seq.setSceneBars(0, 1);
    r.seq.record();
    r.run(kBlock);
    r.seq.noteIn(3, 100);
    r.run(r.loop(1) + kBeat);
    REQUIRE(r.seq.sceneEventLength(0, 0) == 1.0f);   // drawn whole while still held
    r.seq.noteRelease(3);
    r.run(kBlock);
    REQUIRE(r.seq.eventLength(0, 0) == r.loop(1));
}

/* ── A take that is cancelled keeps the scene; a Record take replaces as it
 *    goes and layers from its second pass (UX 2000 section 3.1) ─────────── */

TEST_CASE("sequencer: REC cancelled in its count-in leaves the scene as it was", "[seq][take]") {
    Rig r;
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, 0, 1, 100));
    REQUIRE(r.seq.addEventTicks(0, kTicksPerBeat * 2, 2, 100));
    r.seq.setCountIn(true);
    r.seq.record();
    REQUIRE(r.seq.eventCount(0) == 2);
    r.run(kBeat);
    r.seq.stop();
    REQUIRE(r.seq.eventCount(0) == 2);
}

TEST_CASE("sequencer: a Record pass takes out what it passes, then overdubs", "[seq][take]") {
    Rig r;
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, kTicksPerBeat, 1, 100));        // beat 2
    REQUIRE(r.seq.addEventTicks(0, kTicksPerBeat * 3, 2, 100));    // beat 4
    r.seq.setCountIn(false);
    r.seq.record();
    r.run(kBeat * 2 + kBlock);            // past beat 2, short of beat 4
    REQUIRE(r.seq.eventCount(0) == 1);    // beat 2 gone, beat 4 still there
    REQUIRE(r.fired.empty());             // and the replaced note never sounded
    r.run(kBeat * 2);                     // round the loop
    REQUIRE(r.seq.eventCount(0) == 0);
    REQUIRE(r.seq.mode() == SequencerMode::Overdub);
}

TEST_CASE("sequencer: PLAY after a STOP plays on from where it stopped", "[seq][take]") {
    Rig r;
    r.seq.setSceneBars(0, 1);
    r.seq.playback();
    r.run(kBeat * 2 + kBlock);
    r.seq.stop();
    const uint32_t held = r.clock.positionSamples();
    REQUIRE(held >= kBeat * 2);
    r.seq.playback();
    r.run(kBlock);
    REQUIRE(r.clock.positionSamples() >= held);
    r.seq.stop();
    r.seq.resetClick();
    r.run(kBlock);
    REQUIRE(r.clock.positionSamples() == 0);
}

TEST_CASE("sequencer: out of a take nothing due is skipped", "[seq][take]") {
    Rig r;
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, kTicksPerBeat, 2, 100));
    r.seq.setCountIn(false);
    r.seq.playback();
    r.seq.overdub();
    r.run(kBeat - kBlock);
    /* The clock passes the note's beat with no poll in between -- the poll
     * thread is late -- and PLAY punches out right then. */
    std::vector<float> out(kBlock * 2);
    r.clock.mix(out.data(), kBlock);
    r.clock.mix(out.data(), kBlock);
    r.seq.playback();
    r.seq.poll();
    REQUIRE(r.fired == std::vector<uint8_t>{2});
    r.seq.stop();
}

// ── laid on a song cut into blocks ─────────────────────────────────────────

TEST_CASE("sequencer: a song map plays the scene only under its blocks", "[seq][songmap]") {
    Rig r;
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, 0, 1, 100));
    REQUIRE(r.seq.addEventTicks(0, kTicksPerBeat, 2, 100));
    const uint32_t bar = r.loop(1);
    r.seq.playbackAt(0);
    r.seq.setSongMap({{0, bar, 0}, {2 * bar, 3 * bar, 0}}, 0, 0);
    r.run(3 * bar);
    REQUIRE(r.fired == std::vector<uint8_t>{1, 2, 1, 2});   // bar two is a gap
}

TEST_CASE("sequencer: out of an overdub the song map keeps its place", "[seq][songmap]") {
    Rig r;
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, 0, 1, 100));
    REQUIRE(r.seq.addEventTicks(0, kTicksPerBeat, 2, 100));
    r.seq.setCountIn(false);
    const uint32_t bar = r.loop(1);
    r.seq.playbackAt(0);
    r.seq.setSongMap({{0, bar, 0}, {2 * bar, 3 * bar, 0}}, 0, 0);
    r.seq.overdub();
    r.run(bar + bar / 2);                     // half-way into the gap
    r.seq.playback();                         // PLAY: out of the layering
    r.run(bar + bar / 2);
    /* Taken back to the song's bar one, the gap played bar zero's block. */
    REQUIRE(r.fired == std::vector<uint8_t>{1, 2, 1, 2});
}

TEST_CASE("sequencer: a block plays the scene from its own bar of it", "[seq][songmap]") {
    Rig r;
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, 0, 1, 100));
    REQUIRE(r.seq.addEventTicks(0, kTicksPerBeat, 2, 100));
    const uint32_t bar = r.loop(1);
    r.seq.playbackAt(0);
    r.seq.setSongMap({{0, bar, kBeat}}, 0, 0);   // starts a beat into the scene
    r.run(bar);
    REQUIRE(r.fired == std::vector<uint8_t>{2, 1});
}

TEST_CASE("sequencer: a later map takes over after the samples it was given, the earlier one up to there",
          "[seq][songmap]") {
    Rig r;
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, 0, 1, 100));
    REQUIRE(r.seq.addEventTicks(0, 2 * kTicksPerBeat, 2, 100));
    const uint32_t bar = r.loop(1);
    r.seq.playbackAt(0);
    /* Now: the scene under bar zero only. Later, a beat and a half in: no
     * blocks at all -- the second hit, at beat two, is the later map's. */
    r.seq.setSongMapThen({{0, bar, 0}}, {}, bar * 3 / 8, 0, 0);
    CHECK(r.seq.songMapLaterIn() == bar * 3 / 8);
    r.run(bar);
    CHECK(r.seq.songMapLaterIn() == 0);
    REQUIRE(r.fired == std::vector<uint8_t>{1});
    r.seq.clearSongMap();
    r.seq.stop();
}

TEST_CASE("sequencer: a song map wraps where the song does", "[seq][songmap]") {
    Rig r;
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, 0, 1, 100));
    const uint32_t bar = r.loop(1);
    /* Started in bar two of a song that loops bars 1..2 (0-based): two
     * passes of it are two hits, and bar zero's block is never heard. */
    r.seq.playbackAt(bar);
    r.seq.setSongMap({{0, bar, 0}, {bar, 2 * bar, 0}}, 2 * bar, bar);
    r.run(2 * bar);
    REQUIRE(r.fired.size() == 2);
    r.seq.clearSongMap();
    r.seq.stop();
}

TEST_CASE("sequencer: a block with notes of its own plays them, not the scene", "[seq][songmap]") {
    Rig r;
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, 0, 1, 100));          // the scene: pad 1 on beat one
    const uint32_t bar = r.loop(1);
    auto own = std::make_shared<std::vector<PatternSequencer::SongNote>>();
    own->push_back({0, 0, 5, 100, 0});
    own->push_back({kBeat, 0, 6, 100, 0});
    PatternSequencer::SongSpan block;
    block.a = bar;
    block.b = 3 * bar;
    block.notes = own;
    block.loop = bar;                                     // one bar of notes, played round
    r.seq.playbackAt(0);
    r.seq.setSongMap({{0, bar, 0}, block}, 0, 0);
    r.run(3 * bar);
    REQUIRE(r.fired == std::vector<uint8_t>{1, 5, 6, 5, 6});
}

TEST_CASE("sequencer: a muted song map plays nothing and keeps its place", "[seq][songmap]") {
    Rig r;
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, 0, 1, 100));
    const uint32_t bar = r.loop(1);
    r.seq.playbackAt(0);
    r.seq.setSongMap({{0, 2 * bar, 0}}, 0, 0);
    r.seq.setSongMapMuted(true);
    r.run(bar);
    REQUIRE(r.fired.empty());
    CHECK(r.seq.songMapPosition() == bar);
    r.seq.setSongMapMuted(false);
    r.run(bar);
    REQUIRE(r.fired == std::vector<uint8_t>{1});
}

namespace {
/// What a routed sink was handed: route, pad, on.
struct Routed { uint8_t route; uint8_t pad; bool on; };
std::vector<Routed>* g_routed = nullptr;
void routedSink(void*, uint8_t route, uint8_t pad, uint8_t, bool on, PadPerformance)
{
    if (g_routed) g_routed->push_back({route, pad, on});
}
std::vector<std::pair<uint8_t, uint8_t>> routedOns(const std::vector<Routed>& v)
{
    std::vector<std::pair<uint8_t, uint8_t>> out;
    for (const Routed& r : v) if (r.on) out.push_back({r.route, r.pad});
    return out;
}
} // namespace

TEST_CASE("sequencer: each block plays on its own track's route", "[seq][songmap][route]") {
    Rig r;
    std::vector<Routed> got;
    g_routed = &got;
    r.seq.setSink(&routedSink, nullptr);
    const uint32_t bar = r.loop(1);
    auto a = std::make_shared<std::vector<PatternSequencer::SongNote>>();
    a->push_back({0, 0, 1, 100, 0});
    auto b = std::make_shared<std::vector<PatternSequencer::SongNote>>();
    b->push_back({0, 0, 1, 100, 0});
    PatternSequencer::SongSpan kit;
    kit.b = bar; kit.notes = a; kit.loop = bar;
    PatternSequencer::SongSpan second = kit;
    second.notes = b;
    second.route = 1;
    r.seq.playbackAt(0);
    r.seq.setSongMap({kit, second}, 0, 0);
    r.run(bar);
    /* The same pad on both tracks: two notes, one each, neither cut. */
    REQUIRE(routedOns(got) == std::vector<std::pair<uint8_t, uint8_t>>{{0, 1}, {1, 1}});
    g_routed = nullptr;
}

TEST_CASE("sequencer: a route muted keeps the other playing", "[seq][songmap][route]") {
    Rig r;
    std::vector<Routed> got;
    g_routed = &got;
    r.seq.setSink(&routedSink, nullptr);
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, 0, 2, 100));
    const uint32_t bar = r.loop(1);
    auto own = std::make_shared<std::vector<PatternSequencer::SongNote>>();
    own->push_back({0, 0, 7, 100, 0});
    PatternSequencer::SongSpan first;   // the scene, on the pads' route
    first.b = 2 * bar;
    PatternSequencer::SongSpan second;
    second.b = 2 * bar; second.notes = own; second.loop = bar; second.route = 1;
    r.seq.playbackAt(0);
    r.seq.setSongMap({first, second}, 0, 0);
    r.seq.setSongRoutesMuted(1u << 1);
    r.run(bar);
    REQUIRE(routedOns(got) == std::vector<std::pair<uint8_t, uint8_t>>{{0, 2}});
    got.clear();
    r.seq.setSongRoutesMuted(1u << 0);
    r.run(bar);
    REQUIRE(routedOns(got) == std::vector<std::pair<uint8_t, uint8_t>>{{1, 7}});
    g_routed = nullptr;
}

TEST_CASE("sequencer: a held note lets go on its own route", "[seq][songmap][route]") {
    Rig r;
    std::vector<Routed> got;
    g_routed = &got;
    r.seq.setSink(&routedSink, nullptr);
    const uint32_t bar = r.loop(1);
    auto own = std::make_shared<std::vector<PatternSequencer::SongNote>>();
    own->push_back({0, kBeat, 3, 100, 0});               // held a beat
    PatternSequencer::SongSpan s;
    s.b = bar; s.notes = own; s.loop = bar; s.route = 1;
    r.seq.playbackAt(0);
    r.seq.setSongMap({s}, 0, 0);
    r.run(bar);
    REQUIRE(got.size() == 2);
    CHECK(got[0].route == 1);
    CHECK(got[0].on);
    CHECK(got[1].route == 1);
    CHECK(got[1].pad == 3);
    CHECK_FALSE(got[1].on);
    g_routed = nullptr;
}

TEST_CASE("sequencer: the scene plays on the scene route, and a pads-only sink drops another", "[seq][route]") {
    Rig r;
    std::vector<Routed> got;
    g_routed = &got;
    r.seq.setSink(&routedSink, nullptr);
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, 0, 4, 100));
    const uint32_t bar = r.loop(1);
    r.seq.setSceneRoute(1);
    r.seq.playback();
    r.run(bar);
    REQUIRE(routedOns(got) == std::vector<std::pair<uint8_t, uint8_t>>{{1, 4}});
    r.seq.stop();
    g_routed = nullptr;

    /* A sink that knows only the pads hears nothing of a second kit. */
    Rig plain;
    plain.seq.setSceneBars(0, 1);
    REQUIRE(plain.seq.addEventTicks(0, 0, 4, 100));
    plain.seq.setSceneRoute(1);
    plain.seq.playback();
    plain.run(plain.loop(1));
    REQUIRE(plain.fired.empty());
    plain.seq.setSceneRoute(0);
    plain.run(plain.loop(1));
    REQUIRE(plain.fired == std::vector<uint8_t>{4});
}

TEST_CASE("sequencer: a take onto the song keeps a hit where the song is", "[seq][songtake]") {
    Rig r;
    r.seq.setCountIn(false);
    const uint32_t bar = r.loop(1);
    r.seq.playbackAt(5 * bar);                            // the song's bar six; the scene loops four
    r.seq.setSongMap({}, 0, 0);
    REQUIRE(r.seq.beginSongTake(16, r.seq.activeScene()));
    r.run(kBeat + 100);                                   // a hair after beat two
    r.seq.noteIn(3, 100);
    r.run(kBeat / 2);
    r.seq.noteOut(3);
    r.run(kBlock);
    std::vector<PatternSequencer::SongTakeNote> got;
    r.seq.takeSongNotes(got, true);
    REQUIRE(got.size() == 1);
    CHECK(got[0].pos == 5 * bar + kBeat);                 // on the song's grid
    CHECK(got[0].pad == 3);
    CHECK(got[0].vel == 100);
    CHECK(got[0].len >= kBeat / 2 - kBlock);
    CHECK(got[0].len <= kBeat / 2 + kBlock);
    CHECK(got[0].lap == 0);
    /* Nothing of it went into a scene. */
    CHECK(r.seq.eventCount(0) == 0);
    r.seq.endSongTake();
    r.seq.stop();
}

TEST_CASE("sequencer: a take round a region counts its laps and ends a held note at the wrap", "[seq][songtake]") {
    Rig r;
    r.seq.setCountIn(false);
    const uint32_t bar = r.loop(1);
    r.seq.playbackAt(0);
    r.seq.setSongMap({}, bar, 0);                         // the song goes round its bar one
    REQUIRE(r.seq.beginSongTake(16, r.seq.activeScene()));
    r.run(bar - kBeat);
    r.seq.noteIn(2, 90);                                  // beat four, held over the wrap
    r.run(kBeat * 2);
    CHECK(r.seq.songTakeLaps() == 1);
    r.seq.noteOut(2);
    r.seq.noteIn(4, 80);                                  // lap two, beat two
    r.run(kBlock);
    std::vector<PatternSequencer::SongTakeNote> got;
    r.seq.takeSongNotes(got, true);
    REQUIRE(got.size() == 2);
    CHECK(got[0].pad == 2);
    CHECK(got[0].lap == 0);
    CHECK(got[0].pos == bar - kBeat);
    CHECK(got[0].len == kBeat);                           // to the region's end, no further
    CHECK(got[1].pad == 4);
    CHECK(got[1].lap == 1);
    CHECK(got[1].pos == kBeat);
    r.seq.endSongTake();
    r.seq.stop();
}

// ── note lengths ───────────────────────────────────────────────────────────

namespace {
/// Every press and release the sink sees, with the clock position it came at.
struct Gate {
    struct Ev { uint8_t pad; bool on; uint32_t at; };
    std::vector<Ev> evs;
    uint32_t now = 0;
};
} // namespace

TEST_CASE("sequencer: a held note is recorded with its length and played held", "[seq][length]") {
    Rig r;
    Gate g;
    r.seq.setSink([](void* ctx, uint8_t pad, uint8_t, bool on) {
        auto* gt = static_cast<Gate*>(ctx);
        gt->evs.push_back({pad, on, gt->now});
    }, &g);
    r.seq.setCountIn(false);
    r.seq.setQuantization(Quantization::Off);
    r.seq.setSceneBars(0, 1);
    r.seq.record();
    r.run(kBlock);
    r.seq.noteIn(3, 100);
    r.run(kBeat * 2);                 // held two beats
    r.seq.noteOut(3);
    r.run(kBlock);
    REQUIRE(r.seq.eventCount(0) == 1);
    const uint32_t len = r.seq.eventLength(0, 0);
    REQUIRE(len >= kBeat * 2 - kBlock);
    REQUIRE(len <= kBeat * 2 + kBlock);
    REQUIRE(r.seq.eventLengthTicks(0, 0) == 2 * kTicksPerBeat);

    /* Played: pressed, and released two beats later -- not a poll later. */
    r.seq.stop();
    g.evs.clear();
    r.seq.playbackAt(0);
    uint32_t t = 0;
    while (t < r.loop(1)) { g.now = t; r.run(kBlock); t += kBlock; }
    uint32_t onAt = 0, offAt = 0;
    bool on = false, off = false;
    for (const auto& e : g.evs) {
        if (e.pad != 3) continue;
        if (e.on && !on) { on = true; onAt = e.at; }
        else if (!e.on && on && !off) { off = true; offAt = e.at; }
    }
    REQUIRE(on);
    REQUIRE(off);
    REQUIRE(offAt - onAt >= kBeat * 2 - 2 * kBlock);
    REQUIRE(offAt - onAt <= kBeat * 2 + 2 * kBlock);
    r.seq.stop();
}

TEST_CASE("sequencer: a hit without a release stays a hit", "[seq][length]") {
    Rig r;
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, 0, 1, 100));
    REQUIRE(r.seq.eventLength(0, 0) == 0);
    REQUIRE(r.seq.addEventTicks(0, kTicksPerBeat, 2, 100, kTicksPerBeat / 2));
    REQUIRE(r.seq.eventLength(0, 1) == kBeat / 2);
    REQUIRE(r.seq.eventLengthTicks(0, 1) == kTicksPerBeat / 2);
}

TEST_CASE("sequencer: stopping a take ends a note still held", "[seq][length]") {
    Rig r;
    r.seq.setCountIn(false);
    r.seq.setQuantization(Quantization::Off);
    r.seq.setSceneBars(0, 1);
    r.seq.record();
    r.run(kBlock);
    r.seq.noteIn(5, 100);
    r.run(kBeat);
    r.seq.stop();
    REQUIRE(r.seq.eventCount(0) == 1);
    REQUIRE(r.seq.eventLength(0, 0) > 0);
    r.seq.noteOut(5);                 // after the stop: nothing open, nothing changes
    r.seq.poll();
    REQUIRE(r.seq.eventCount(0) == 1);
}

TEST_CASE("sequencer: a tempo change keeps a note's length in beats", "[seq][length]") {
    Rig r;
    r.seq.setSceneBars(0, 1);
    REQUIRE(r.seq.addEventTicks(0, 0, 1, 100, kTicksPerBeat));
    r.seq.setClickBpm(60.0f);
    REQUIRE(r.seq.eventLengthTicks(0, 0) == kTicksPerBeat);
    REQUIRE(r.seq.eventLength(0, 0) == kBeat * 2);
}

TEST_CASE("sequencer: note lengths travel in the sequence file, hits stay triples", "[seq][length]") {
    const char* path = "cp_seq_len.json";
    Rig a;
    a.seq.setSceneBars(0, 1);
    REQUIRE(a.seq.addEventTicks(0, 0, 1, 100));
    REQUIRE(a.seq.addEventTicks(0, kTicksPerBeat, 2, 90, 3 * kTicksPerBeat / 2));
    REQUIRE(SequenceStore::save(a.seq, path));

    Rig b;
    REQUIRE(SequenceStore::load(b.seq, path));
    std::remove(path);
    REQUIRE(b.seq.eventCount(0) == 2);
    REQUIRE(b.seq.eventLengthTicks(0, 0) == 0);
    REQUIRE(b.seq.eventLengthTicks(0, 1) == 3 * kTicksPerBeat / 2);
}

TEST_CASE("sequencer: a 16 levels take keeps what each hit sounded as", "[seq][levels]") {
    /* What the pad logic says a hit is when it comes in -- here 16 levels of
     * pitch on pad 4, with Full level -- is kept with the hit and handed back
     * on playback, whatever the pads would make of it by then. */
    struct Perf {
        static PadPerformance of(void*, uint8_t, uint8_t& vel) {
            vel = 127;
            PadPerformance p;
            p.levels = PadLevels::Pitch;
            p.source = 4;
            return p;
        }
    };
    struct Heard { uint8_t pad; uint8_t vel; PadPerformance perf; };
    std::vector<Heard> heard;

    Rig r;
    r.seq.setPerformanceSource(&Perf::of, nullptr);
    r.seq.setSink([](void* ctx, uint8_t pad, uint8_t vel, bool on, PadPerformance perf) {
        if (on) static_cast<std::vector<Heard>*>(ctx)->push_back({pad, vel, perf});
    }, &heard);
    r.seq.setCountIn(false);
    r.seq.setSceneBars(0, 1);
    r.seq.record();
    r.run(kBlock);
    r.seq.noteIn(7, 40);                  // pad 7 is the source, a fifth up
    r.run(kBlock);
    r.seq.noteOut(7);
    r.run(kBlock);
    REQUIRE(r.seq.eventCount(0) == 1);
    uint32_t pos = 0; uint8_t pad = 0, vel = 0;
    REQUIRE(r.seq.eventAt(0, 0, pos, pad, vel));
    REQUIRE(pad == 7);                    // the lane is the level that was hit
    REQUIRE(vel == 127);                  // at the velocity that sounded
    REQUIRE(r.seq.eventPerformance(0, 0).levels == PadLevels::Pitch);
    REQUIRE(r.seq.eventPerformance(0, 0).source == 4);

    /* Played back with the source gone: what was recorded, not what the pads
     * would make of pad 7 now. */
    r.seq.setPerformanceSource(nullptr, nullptr);
    r.seq.stop();
    heard.clear();
    r.seq.playbackAt(0);
    r.run(r.loop(1));
    r.seq.stop();
    REQUIRE_FALSE(heard.empty());
    REQUIRE(heard[0].pad == 7);
    REQUIRE(heard[0].vel == 127);
    REQUIRE(heard[0].perf.levels == PadLevels::Pitch);
    REQUIRE(heard[0].perf.source == 4);
}

TEST_CASE("sequence file: a hit's performance travels with it", "[seq][store][levels]") {
    const char* path = "cp_seq_levels.json";
    PadPerformance pitch;
    pitch.levels = PadLevels::Pitch;
    pitch.source = 9;
    PadPerformance velo;
    velo.levels = PadLevels::Velocity;
    velo.source = 2;

    Rig a;
    a.seq.setSceneBars(0, 1);
    REQUIRE(a.seq.addEventTicks(0, 0, 3, 100));                         // plain hit
    REQUIRE(a.seq.addEventTicks(0, kTicksPerBeat, 5, 90, 0, pitch));    // a hit at a level
    REQUIRE(a.seq.addEventTicks(0, kTicksPerBeat * 2, 6, 47, kTicksPerBeat, velo));
    REQUIRE(SequenceStore::save(a.seq, path));

    Rig b;
    REQUIRE(SequenceStore::load(b.seq, path));
    std::remove(path);
    REQUIRE(b.seq.eventCount(0) == 3);
    REQUIRE(b.seq.eventPerformance(0, 0).plain());
    REQUIRE(b.seq.eventPerformance(0, 1).levels == PadLevels::Pitch);
    REQUIRE(b.seq.eventPerformance(0, 1).source == 9);
    REQUIRE(b.seq.eventLengthTicks(0, 1) == 0);                         // still a hit
    REQUIRE(b.seq.eventPerformance(0, 2).levels == PadLevels::Velocity);
    REQUIRE(b.seq.eventPerformance(0, 2).source == 2);
    REQUIRE(b.seq.eventLengthTicks(0, 2) == kTicksPerBeat);
}

TEST_CASE("PadPerformance packs into one byte and back", "[seq][levels]") {
    PadPerformance p;
    REQUIRE(p.pack() == 0);
    p.levels = PadLevels::Pitch;
    p.source = 15;
    REQUIRE(PadPerformance::unpack(p.pack()).levels == PadLevels::Pitch);
    REQUIRE(PadPerformance::unpack(p.pack()).source == 15);
    REQUIRE(PadPerformance::unpack(0x70).plain());     // a mode it does not know
}

TEST_CASE("sequencer: swing is a scene's own", "[seq][swing]") {
    Rig r;
    r.seq.setSceneCount(2);
    r.seq.selectScene(0);
    r.seq.setSwing(67);
    r.seq.setSwingPerBeat(4);
    CHECK(r.seq.sceneSwing(0) == 67);
    CHECK(r.seq.sceneSwingPerBeat(0) == 4);
    CHECK(r.seq.sceneSwing(1) == 0);                     // the other scene stays straight
    CHECK(r.seq.sceneSwingPerBeat(1) == 2);
    r.seq.setSceneSwing(1, 200, 3);                      // clamped; 3 keeps the subdivision it had
    CHECK(r.seq.sceneSwing(1) == PatternSequencer::kMaxSwing);
    CHECK(r.seq.sceneSwingPerBeat(1) == 2);
    r.seq.selectScene(1);
    CHECK(r.seq.swing() == PatternSequencer::kMaxSwing);

    /* A scene removed takes its swing with it; the one after moves down. */
    r.seq.removeScene(0);
    CHECK(r.seq.sceneSwing(0) == PatternSequencer::kMaxSwing);
    CHECK(r.seq.sceneSwing(1) == 0);
}

TEST_CASE("sequence file: each scene's swing travels, a file without it loads straight", "[seq][swing][store]") {
    const std::string path = "cp_seq_swing.json";
    removeAll(path);
    Rig a;
    a.seq.setSceneCount(2);
    a.seq.setSceneSwing(1, 48, 4);
    REQUIRE(SequenceStore::save(a.seq, path));
    Rig b;
    b.seq.setSceneSwing(0, 30, 2);                       // what it held is replaced
    REQUIRE(SequenceStore::load(b.seq, path));
    CHECK(b.seq.sceneSwing(0) == 0);
    CHECK(b.seq.sceneSwing(1) == 48);
    CHECK(b.seq.sceneSwingPerBeat(1) == 4);

    writeText(path, "{\"version\":1,\"ppq\":96,\"bpm\":120,\"scenes\":[{\"bars\":1,\"events\":[[0,3,90]]}]}");
    Rig c;
    REQUIRE(SequenceStore::load(c.seq, path));
    CHECK(c.seq.sceneSwing(0) == 0);
    removeAll(path);
}

TEST_CASE("sequencer: a song's blocks play with their own track's swing", "[seq][swing][songmap]") {
    Rig r;
    r.seq.setSceneCount(2);
    r.seq.setSceneSwing(0, 100, 2);                      // eighths, the off-beat a quarter beat late
    r.seq.setSceneSwing(1, 0, 2);
    const uint32_t bar = r.loop(1);
    auto offBeat = [](uint8_t pad) {
        auto n = std::make_shared<std::vector<PatternSequencer::SongNote>>();
        n->push_back({kBeat / 2, 0, pad, 100, 0});       // the first off-beat eighth
        return n;
    };
    PatternSequencer::SongSpan swung;
    swung.b = bar; swung.notes = offBeat(1); swung.loop = bar; swung.swingScene = 0;
    PatternSequencer::SongSpan straight = swung;
    straight.notes = offBeat(2);
    straight.swingScene = 1;
    r.seq.playbackAt(0);
    r.seq.setSongMap({swung, straight}, 0, 0);
    r.run(kBeat / 2 + kBlock);
    CHECK(r.fired == std::vector<uint8_t>{2});           // the straight one on time, the swung one not yet
    r.run(kBeat / 4);
    CHECK(r.fired == std::vector<uint8_t>{2, 1});
}

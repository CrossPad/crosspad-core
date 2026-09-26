// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include "crosspad/sequencer/PatternSequencer.hpp"

#include <atomic>
#include <thread>
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

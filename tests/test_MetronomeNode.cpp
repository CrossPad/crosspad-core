// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "crosspad/sequencer/MetronomeNode.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace crosspad;

namespace {
constexpr uint32_t kRate  = 48000;
constexpr uint32_t kBlock = 256;

void prepared(MetronomeNode& m)
{
    m.onPrepare(kBlock, kRate);
    m.setBpm(120.0f);   // 24000 samples a beat
}

void render(MetronomeNode& m, uint32_t frames = kBlock)
{
    std::vector<float> out(frames * 2, 0.0f);
    m.mix(out.data(), frames);
}
} // namespace

TEST_CASE("metronome: startAt starts at that position", "[metronome]") {
    MetronomeNode m;
    prepared(m);
    m.startAt(1000);
    REQUIRE(m.running());
    REQUIRE(m.positionSamples() == 1000);
    render(m);
    REQUIRE(m.positionSamples() == 1000 + kBlock);
}

TEST_CASE("metronome: startAt tells the transport hook", "[metronome]") {
    MetronomeNode m;
    prepared(m);
    int starts = 0;
    m.setTransportHook([](bool running, void* ctx) { if (running) ++*static_cast<int*>(ctx); }, &starts);
    m.startAt(5000);
    REQUIRE(starts == 1);
}

TEST_CASE("metronome: a count-in holds the loop at zero, then hands over", "[metronome]") {
    MetronomeNode m;
    prepared(m);
    m.startCountIn(2);
    REQUIRE(m.countingIn());
    REQUIRE(m.countInBeatsLeft() == 2);
    render(m, 24000);
    REQUIRE(m.countInBeatsLeft() == 1);
    REQUIRE(m.positionSamples() == 0);
    render(m, 24000 + 100);
    REQUIRE_FALSE(m.countingIn());
    REQUIRE(m.positionSamples() == 100);
}

TEST_CASE("metronome: the hook hears a count-in start at beat one, not before", "[metronome]") {
    MetronomeNode m;
    prepared(m);
    std::vector<int> seen;
    m.setTransportHook([](bool running, void* ctx) {
        static_cast<std::vector<int>*>(ctx)->push_back(running ? 1 : 0);
    }, &seen);
    m.startCountIn(1);
    REQUIRE(seen.empty());
    REQUIRE_FALSE(m.takeCountInEnded());
    render(m, 24000 + 10);
    REQUIRE(m.takeCountInEnded());
    REQUIRE(seen == std::vector<int>{1});
    REQUIRE_FALSE(m.takeCountInEnded());        // once
    REQUIRE(seen == std::vector<int>{1});
}

TEST_CASE("metronome: stop during a count-in leaves nothing to count", "[metronome]") {
    MetronomeNode m;
    prepared(m);
    m.startCountIn(4);
    render(m);
    m.stop();
    render(m);
    REQUIRE_FALSE(m.running());
    REQUIRE_FALSE(m.countingIn());
    m.start();
    REQUIRE_FALSE(m.countingIn());
    render(m);
    REQUIRE(m.positionSamples() == kBlock);
}

TEST_CASE("metronome: resume after a stopped count-in does not count again", "[metronome]") {
    MetronomeNode m;
    prepared(m);
    m.startCountIn(4);
    render(m);
    m.stop();
    m.resume();
    REQUIRE_FALSE(m.countingIn());
}

TEST_CASE("metronome: a transport change is never undone by the block in flight", "[metronome][rt]") {
    // The render thread runs blocks back to back while this thread keeps
    // moving the transport. Every move must be where the next reads find it:
    // a block that loaded the old position and stored it back afterwards
    // would put the clock on the old timeline.
    //
    // The clock does not wait for this thread, so a reading is judged against
    // the blocks that actually ran since the move, not against a fixed
    // distance: a scheduler that parks this thread for a few hundred blocks
    // moves a correct clock that far too.
    MetronomeNode m;
    prepared(m);
    m.start();
    std::atomic<bool> done{false};
    std::atomic<uint32_t> blocks{0};
    std::thread rt([&] {
        std::vector<float> out(kBlock * 2);
        while (!done.load(std::memory_order_relaxed)) {
            m.mix(out.data(), kBlock);
            blocks.fetch_add(1, std::memory_order_release);
        }
    });

    constexpr int kMoves = 2000;
    constexpr uint32_t kFar = 200000;          // well inside the default 4-bar loop (384000 samples)
    constexpr uint32_t kSettleBlocks = 3;      // the move lands at the next block start
    const uint32_t loop = m.loopSamples();
    int wrong = 0;
    for (int i = 0; i < kMoves; ++i) {
        const uint32_t target = (i & 1) ? kFar : 0;
        const uint32_t before = blocks.load(std::memory_order_acquire);
        const auto off = [&] {
            const uint32_t p = m.positionSamples();
            const uint32_t ran = blocks.load(std::memory_order_acquire) - before;
            // +1: the block in flight may have stored its position before counting.
            return (p + loop - target) % loop > (ran + 1) * kBlock;
        };
        m.startAt(target);
        // Read at once too: the block that takes the move must not show the
        // old position while it renders.
        if (off()) ++wrong;
        const uint32_t b0 = blocks.load(std::memory_order_acquire);
        while (blocks.load(std::memory_order_acquire) - b0 < kSettleBlocks) std::this_thread::yield();
        if (off()) ++wrong;
    }
    done.store(true);
    rt.join();
    REQUIRE(wrong == 0);
}

namespace {
/// The samples a click starts on over @p frames: where the output turns from
/// silence to sound. The first sample of a click is sin(0) = 0, so the onset
/// is the sample before the first non-zero one.
std::vector<uint32_t> onsets(MetronomeNode& m, uint32_t frames, std::vector<float>* peaks = nullptr)
{
    std::vector<float> out(frames * 2, 0.0f);
    m.mix(out.data(), frames);
    std::vector<uint32_t> at;
    bool sounding = false;
    float peak = 0.0f;
    for (uint32_t i = 0; i < frames; ++i) {
        const float v = out[i * 2] < 0 ? -out[i * 2] : out[i * 2];
        if (v > 0.0f && !sounding) {
            if (peaks && !at.empty()) peaks->push_back(peak);
            at.push_back(i - 1);
            peak = 0.0f;
        }
        sounding = v > 0.0f;
        if (v > peak) peak = v;
    }
    if (peaks && !at.empty()) peaks->push_back(peak);
    return at;
}
} // namespace

TEST_CASE("metronome: a division clicks between the beats, quieter", "[metronome]") {
    MetronomeNode m;
    prepared(m);
    m.setDivision(4);
    REQUIRE(m.division() == 4);
    m.start();
    std::vector<float> peaks;
    const std::vector<uint32_t> at = onsets(m, 48000, &peaks);   // two beats
    REQUIRE(at == std::vector<uint32_t>{0, 6000, 12000, 18000, 24000, 30000, 36000, 42000});
    REQUIRE(peaks.size() == at.size());
    REQUIRE(peaks[1] < peaks[0] * 0.6f);
    REQUIRE(peaks[4] > peaks[5]);
}

TEST_CASE("metronome: a beat that does not divide evenly has no tick just before it", "[metronome]") {
    MetronomeNode m;
    m.onPrepare(kBlock, 44100);
    m.setBpm(120.0f);           // 22050 samples a beat: not a multiple of 4
    m.setDivision(4);
    m.start();
    const std::vector<uint32_t> at = onsets(m, 22050 + 100);
    REQUIRE(at.size() == 5);    // four in the beat, then the next beat's
    REQUIRE(at.back() == 22050);
    for (size_t i = 1; i < at.size(); ++i) REQUIRE(at[i] - at[i - 1] >= 5512);
}

TEST_CASE("metronome: an unknown division is quarters, and the level is read back", "[metronome]") {
    MetronomeNode m;
    m.setDivision(3);
    REQUIRE(m.division() == 1);
    m.setLevel(0.7f);
    REQUIRE(m.level() == 0.7f);
    m.setLevel(2.0f);
    REQUIRE(m.level() == 1.0f);
}

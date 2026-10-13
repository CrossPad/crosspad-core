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

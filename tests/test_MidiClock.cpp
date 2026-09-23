// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "crosspad/midi/MidiClock.hpp"

#include <cmath>
#include <cstdint>

using namespace crosspad;
using Catch::Matchers::WithinRel;

namespace {

constexpr double periodUs(double bpm) { return 60e6 / (24.0 * bpm); }

/// Uniform ±1 ms (what USB frames and host scheduling do to a host clock),
/// from a fixed-seed LCG so a failure reproduces.
int32_t jitterUs(uint32_t& seed) {
    seed = seed * 1664525u + 1013904223u;
    return (int32_t)(seed >> 8) % 2001 - 1000;
}

/// Feed `n` host ticks at `bpm` starting at `t0`. Returns the last tick's time.
int64_t feed(MidiClock& c, double bpm, int64_t t0, int n, bool jitter = true,
             uint32_t seed = 12345u) {
    int64_t t = t0;
    for (int i = 0; i < n; ++i) {
        t = t0 + (int64_t)std::llround(i * periodUs(bpm)) + (jitter ? jitterUs(seed) : 0);
        c.hostTick(t);
    }
    return t;
}

} // namespace

TEST_CASE("follower reads the host tempo within 0.1 %", "[midiclock]") {
    for (double bpm : {90.0, 120.0, 174.0}) {
        for (uint32_t seed : {1u, 7u, 12345u, 99991u}) {
            MidiClock c(100.0f, 0);
            feed(c, bpm, 1000, 97, true, seed);   // four beats: the full window
            REQUIRE(c.source() == MidiClock::Source::Host);
            REQUIRE_THAT(c.bpm(), WithinRel((float)bpm, 0.001f));
        }
    }
}

TEST_CASE("host released after 1 s without a tick, tempo kept", "[midiclock]") {
    MidiClock c(100.0f, 0);
    const int64_t last = feed(c, 128.0, 1000, 97, false);
    REQUIRE_FALSE(c.poll(last + MidiClock::kHostTimeoutUs - 1));
    REQUIRE(c.source() == MidiClock::Source::Host);
    REQUIRE(c.poll(last + MidiClock::kHostTimeoutUs + 1));
    REQUIRE(c.source() == MidiClock::Source::Internal);
    REQUIRE_THAT(c.bpm(), WithinRel(128.0f, 0.001f));
    REQUIRE_FALSE(c.poll(last + MidiClock::kHostTimeoutUs + 2));   // once
}

TEST_CASE("restart gap drops the old window", "[midiclock]") {
    MidiClock c(100.0f, 0);
    const int64_t last = feed(c, 120.0, 1000, 97, false);
    // DAW stopped, tempo changed, plays again 300 ms later (< 1 s timeout)
    feed(c, 90.0, last + 300000, 97, false);
    REQUIRE_THAT(c.bpm(), WithinRel(90.0f, 0.001f));
}

TEST_CASE("start keeps the host source alive until the first tick", "[midiclock]") {
    MidiClock c(100.0f, 0);
    const int64_t last = feed(c, 120.0, 1000, 49, false);
    c.hostStop(last + 10000);
    REQUIRE_FALSE(c.running());
    c.hostStart(last + 900000);                        // count-in: no tick yet
    REQUIRE(c.running());
    REQUIRE_FALSE(c.poll(last + 1500000));             // 0.6 s after FA
    REQUIRE(c.source() == MidiClock::Source::Host);
    c.hostContinue(last + 1600000);
    REQUIRE(c.running());
}

TEST_CASE("manual tempo is ignored while the host drives", "[midiclock]") {
    MidiClock c(100.0f, 0);
    feed(c, 120.0, 1000, 97, false);
    c.setBpm(80.0f, 2000000);
    REQUIRE_THAT(c.bpm(), WithinRel(120.0f, 0.001f));
}

TEST_CASE("generator is drift-free over ten minutes at 174 BPM", "[midiclock]") {
    MidiClock c(174.0f, 5000);
    const uint64_t n = 24ull * 174ull * 10ull;         // 10 minutes of ticks
    for (uint64_t i = 0; i < n; ++i) {
        const int64_t due = c.nextTickDue();
        c.internalTickSent(due + (int64_t)(i % 3) * 700);   // sent up to 1.4 ms late
    }
    const double exact = 5000.0 + (double)n * periodUs(174.0);
    REQUIRE(std::fabs((double)c.nextTickDue() - exact) < 1.0);
    REQUIRE(c.ticksOut() == n);
    REQUIRE(c.maxLateUs() == 1400);
}

TEST_CASE("a BPM change restarts the count from the next due tick", "[midiclock]") {
    MidiClock c(120.0f, 0);
    c.internalTickSent(c.nextTickDue());
    const int64_t due = c.nextTickDue();
    c.setBpm(60.0f, due - 100);
    REQUIRE(c.nextTickDue() == due);
    c.internalTickSent(due);
    REQUIRE(c.nextTickDue() == due + (int64_t)std::llround(periodUs(60.0)));
}

TEST_CASE("a late generator restarts instead of bursting", "[midiclock]") {
    MidiClock c(120.0f, 0);
    const int64_t due = c.nextTickDue();
    const int64_t now = due + 3 * (int64_t)periodUs(120.0);   // stalled three ticks
    c.internalTickSent(now);
    REQUIRE(c.nextTickDue() >= now);
}

TEST_CASE("tempo changes are published once per real change", "[midiclock]") {
    MidiClock c(120.0f, 0);
    float bpm = 0; MidiClock::Source src{};
    REQUIRE(c.takeTempoChange(bpm, src));              // initial state
    REQUIRE_FALSE(c.takeTempoChange(bpm, src));
    const int64_t last = feed(c, 120.0, 1000, 97, false);
    REQUIRE(c.takeTempoChange(bpm, src));              // source became Host
    REQUIRE(src == MidiClock::Source::Host);
    feed(c, 120.0, last + (int64_t)periodUs(120.0), 48, false);
    REQUIRE_FALSE(c.takeTempoChange(bpm, src));        // steady host: nothing new
    c.poll(last + 5000000);
    REQUIRE(c.takeTempoChange(bpm, src));              // back to Internal
    REQUIRE(src == MidiClock::Source::Internal);
}

TEST_CASE("BPM is clamped to 20..300", "[midiclock]") {
    MidiClock c(1000.0f, 0);
    REQUIRE(c.bpm() == MidiClock::kMaxBpm);
    c.setBpm(std::nanf(""), 0);
    REQUIRE(c.bpm() == MidiClock::kMinBpm);
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "crosspad/dsp/ScopeTrigger.hpp"

#include <cmath>
#include <vector>

using namespace crosspad;

namespace {

constexpr uint32_t kSr441 = 44100;
constexpr uint32_t kSr48 = 48000;
constexpr float kTwoPi = 6.28318530718f;

std::vector<float> sine(float hz, uint32_t sampleRate, uint32_t n, float phase,
                        float amp = 0.8f) {
    std::vector<float> v(n);
    for (uint32_t i = 0; i < n; ++i) {
        v[i] = amp * std::sin(kTwoPi * hz * static_cast<float>(i)
                              / static_cast<float>(sampleRate) + phase);
    }
    return v;
}

/// Distance from `start` to the nearest rising zero-crossing of the sine, in
/// samples — the quantity the trigger is supposed to keep under one sample.
float crossingError(uint32_t start, float hz, uint32_t sampleRate, float phase) {
    // Rising crossings sit where the argument is a whole number of turns.
    const float turns = (kTwoPi * hz * static_cast<float>(start)
                         / static_cast<float>(sampleRate) + phase) / kTwoPi;
    const float ideal = (std::round(turns) * kTwoPi - phase)
                      * static_cast<float>(sampleRate) / (kTwoPi * hz);
    return std::fabs(static_cast<float>(start) - ideal);
}

} // namespace

TEST_CASE("scopeWindowSamples: three periods, clamped to 1..10 ms", "[dsp][scope]") {
    // 100 Hz: three periods is 30 ms, far past the ceiling.
    REQUIRE(scopeWindowSamples(100.0f, kSr441) == 441u);
    REQUIRE(scopeWindowSamples(100.0f, kSr48) == 480u);

    // Middle C, 261.6 Hz: 11.5 ms, still over the ceiling.
    REQUIRE(scopeWindowSamples(261.6f, kSr441) == 441u);
    REQUIRE(scopeWindowSamples(261.6f, kSr48) == 480u);

    // 1 kHz: 3 ms, inside the band — this is where the fundamental decides.
    REQUIRE(scopeWindowSamples(1000.0f, kSr441) == 132u);
    REQUIRE(scopeWindowSamples(1000.0f, kSr48) == 144u);

    // 5 kHz: 0.6 ms, under the floor.
    REQUIRE(scopeWindowSamples(5000.0f, kSr441) == 44u);
    REQUIRE(scopeWindowSamples(5000.0f, kSr48) == 48u);

    // 300 Hz is exactly the ceiling; A440 is comfortably inside.
    REQUIRE(scopeWindowSamples(300.0f, kSr441) == 441u);
    REQUIRE(scopeWindowSamples(440.0f, kSr441) == 301u);
}

TEST_CASE("scopeWindowSamples: no note takes the widest window", "[dsp][scope]") {
    REQUIRE(scopeWindowSamples(0.0f, kSr441) == 441u);
    REQUIRE(scopeWindowSamples(-440.0f, kSr441) == 441u);
    REQUIRE(scopeWindowSamples(440.0f, 0) == 0u);
}

TEST_CASE("scopeTrigger: a sine triggers on a rising crossing", "[dsp][scope]") {
    const float hz = 440.0f;
    const uint32_t length = scopeWindowSamples(hz, kSr441);

    for (float phase : {0.0f, 0.7f, 2.5f, 4.9f}) {
        const std::vector<float> buf = sine(hz, kSr441, 4096, phase);
        const ScopeWindow w = scopeTrigger(buf.data(), 4096, length);

        REQUIRE(w.triggered);
        REQUIRE(w.length == length);
        REQUIRE(w.start + w.length <= 4096u);
        REQUIRE(buf[w.start - 1] < 0.0f);
        REQUIRE(buf[w.start] >= 0.0f);
        REQUIRE(crossingError(w.start, hz, kSr441, phase) <= 1.0f);
    }
}

TEST_CASE("scopeTrigger: two snapshots an odd distance apart show one phase",
          "[dsp][scope]") {
    // The whole point of the trigger: the UI redraws on its own clock, which
    // has nothing to do with the note period, and the wave must still stand
    // still. 250 samples at 440 Hz / 44.1 kHz is 2.49 periods — deliberately
    // not a whole number, so an untriggered view would slide half a cycle.
    const float hz = 440.0f;
    const float period = static_cast<float>(kSr441) / hz;
    const uint32_t length = scopeWindowSamples(hz, kSr441);
    const uint32_t view = 4096;
    const uint32_t offset = 250;

    const std::vector<float> full = sine(hz, kSr441, view + offset, 0.0f);
    const ScopeWindow a = scopeTrigger(full.data(), view, length);
    const ScopeWindow b = scopeTrigger(full.data() + offset, view, length);
    REQUIRE(a.triggered);
    REQUIRE(b.triggered);

    // Both starts expressed in the same (absolute) timeline, folded onto one
    // period: the same phase, not the same index.
    const float absA = static_cast<float>(a.start);
    const float absB = static_cast<float>(b.start + offset);
    float d = std::fmod(std::fabs(absA - absB), period);
    if (d > period * 0.5f) d = period - d;
    REQUIRE(d <= 1.0f);
}

TEST_CASE("scopeTrigger: silence falls back to the tail", "[dsp][scope]") {
    const std::vector<float> quiet(1024, 0.0f);
    const ScopeWindow w = scopeTrigger(quiet.data(), 1024, 301);
    REQUIRE_FALSE(w.triggered);
    REQUIRE(w.length == 301u);
    REQUIRE(w.start == 1024u - 301u);
}

TEST_CASE("scopeTrigger: hysteresis ignores the noise floor", "[dsp][scope]") {
    // +-0.001 alternating crosses zero on every single sample; without the
    // band the trigger would latch onto the newest of them and the view would
    // jitter precisely when nothing is playing.
    std::vector<float> noise(1024);
    for (uint32_t i = 0; i < 1024; ++i) noise[i] = (i % 2) ? 0.001f : -0.001f;
    const ScopeWindow quiet = scopeTrigger(noise.data(), 1024, 64);
    REQUIRE_FALSE(quiet.triggered);
    REQUIRE(quiet.start == 1024u - 64u);

    // Ten times the amplitude is over the band and does trigger, so the
    // rejection above is the hysteresis and not a dead function.
    std::vector<float> loud(1024);
    for (uint32_t i = 0; i < 1024; ++i) loud[i] = (i % 2) ? 0.01f : -0.01f;
    const ScopeWindow hit = scopeTrigger(loud.data(), 1024, 64);
    REQUIRE(hit.triggered);
    REQUIRE(hit.start == 959u);          // the newest rising edge that still fits
}

TEST_CASE("scopeTrigger: degenerate arguments stay inside the buffer", "[dsp][scope]") {
    const std::vector<float> buf = sine(440.0f, kSr441, 100, 0.0f);

    // A window longer than the snapshot is shortened, never honoured: the
    // caller draws [start, start + length) out of a buffer it owns.
    const ScopeWindow small = scopeTrigger(buf.data(), 100, 301);
    REQUIRE_FALSE(small.triggered);
    REQUIRE(small.start == 0u);
    REQUIRE(small.length == 100u);

    REQUIRE(scopeTrigger(nullptr, 100, 32).length == 0u);
    REQUIRE(scopeTrigger(buf.data(), 0, 32).length == 0u);
    REQUIRE(scopeTrigger(buf.data(), 100, 0).length == 0u);
}

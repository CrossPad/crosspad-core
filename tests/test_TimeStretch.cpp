// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "crosspad/dsp/TimeStretch.hpp"

#include <cmath>
#include <cstring>
#include <vector>

using crosspad::TimeStretch;

namespace {

constexpr double kRate = 44100.0;
constexpr double kPi = 3.14159265358979323846;

struct Run {
    std::vector<int16_t> in;     // stereo
    std::vector<int16_t> out;    // stereo
    uint32_t stopAt = 0;         // keep() says stop once this many are out; 0 = never
    std::vector<uint8_t> work = std::vector<uint8_t>(TimeStretch::workBytes());

    static bool read(void* c, uint32_t frame, int16_t* dst, uint32_t n) {
        auto* r = static_cast<Run*>(c);
        std::memcpy(dst, r->in.data() + (size_t)frame * 2, (size_t)n * 4);
        return true;
    }
    static bool write(void* c, const int16_t* src, uint32_t n) {
        auto* r = static_cast<Run*>(c);
        r->out.insert(r->out.end(), src, src + (size_t)n * 2);
        return true;
    }
    static bool keep(void* c, uint32_t done, uint32_t) {
        auto* r = static_cast<Run*>(c);
        return !r->stopAt || done < r->stopAt;
    }
    bool go(uint32_t outFrames) {
        return TimeStretch::run((uint32_t)(in.size() / 2), outFrames, work.data(), read, write, keep, this);
    }
    bool repitch(uint32_t outFrames) {
        return TimeStretch::repitch((uint32_t)(in.size() / 2), outFrames, work.data(), read, write, keep, this);
    }
};

std::vector<int16_t> sine(double hz, uint32_t frames, double amp = 0.5)
{
    std::vector<int16_t> v((size_t)frames * 2);
    for (uint32_t i = 0; i < frames; ++i) {
        const int16_t s = (int16_t)std::lround(amp * 32767.0 * std::sin(2.0 * kPi * hz * i / kRate));
        v[i * 2] = v[i * 2 + 1] = s;
    }
    return v;
}

/// Frequency of the left channel over [a, b), from its rising zero crossings.
double frequency(const std::vector<int16_t>& v, uint32_t a, uint32_t b)
{
    int first = -1, last = -1, n = 0;
    for (uint32_t i = a + 1; i < b; ++i) {
        if (v[(i - 1) * 2] < 0 && v[i * 2] >= 0) {
            if (first < 0) first = (int)i;
            last = (int)i;
            ++n;
        }
    }
    return n > 1 ? (n - 1) * kRate / (last - first) : 0.0;
}

} // namespace

TEST_CASE("time stretch: exactly the frames asked for", "[dsp][stretch]") {
    for (double ratio : {0.8, 0.95, 1.0, 1.07, 1.25, 1.6}) {
        Run r;
        r.in = sine(220.0, 44100);
        const uint32_t want = (uint32_t)std::lround(44100 * ratio);
        REQUIRE(r.go(want));
        REQUIRE(r.out.size() == (size_t)want * 2);
    }
}

TEST_CASE("time stretch: the pitch stays where it was", "[dsp][stretch]") {
    for (double ratio : {0.8, 1.25}) {
        Run r;
        r.in = sine(1000.0, 44100);
        const uint32_t want = (uint32_t)std::lround(44100 * ratio);
        REQUIRE(r.go(want));
        const double f = frequency(r.out, 2000, want - 2000);
        REQUIRE_THAT(f, Catch::Matchers::WithinRel(1000.0, 0.01));
    }
}

TEST_CASE("time stretch: the clip starts on its own attack", "[dsp][stretch]") {
    Run r;
    r.in = sine(330.0, 22050, 0.8);
    REQUIRE(r.go(26000));
    for (uint32_t i = 0; i < TimeStretch::kHop * 2; ++i) REQUIRE(r.out[i] == r.in[i]);
}

TEST_CASE("time stretch: silence in, silence out", "[dsp][stretch]") {
    Run r;
    r.in.assign(44100 * 2, 0);
    REQUIRE(r.go(50000));
    for (int16_t s : r.out) REQUIRE(s == 0);
}

TEST_CASE("time stretch: a steady tone keeps its level", "[dsp][stretch]") {
    Run r;
    r.in = sine(440.0, 44100, 0.5);
    REQUIRE(r.go(52000));
    double sum = 0.0;
    for (uint32_t i = 4000; i < 48000; ++i) sum += (double)r.out[i * 2] * r.out[i * 2];
    const double rms = std::sqrt(sum / 44000.0) / 32767.0;
    REQUIRE_THAT(rms, Catch::Matchers::WithinRel(0.5 / std::sqrt(2.0), 0.05));
}

TEST_CASE("time stretch: refuses what it cannot do, and stops when told", "[dsp][stretch]") {
    Run r;
    r.in = sine(220.0, 10000);
    REQUIRE_FALSE(r.go(4000));      // ratio 0.4
    REQUIRE_FALSE(r.go(25000));     // ratio 2.5
    r.out.clear();
    r.stopAt = 2000;
    REQUIRE_FALSE(r.go(12000));
    REQUIRE(r.out.size() < (size_t)12000 * 2);
}

TEST_CASE("repitch: exactly the frames asked for", "[dsp][stretch]") {
    for (double ratio : {0.5, 0.8, 1.0, 1.07, 1.25, 2.0}) {
        Run r;
        r.in = sine(220.0, 44100);
        const uint32_t want = (uint32_t)std::lround(44100 * ratio);
        REQUIRE(r.repitch(want));
        REQUIRE(r.out.size() == (size_t)want * 2);
    }
}

TEST_CASE("repitch: the pitch follows the length", "[dsp][stretch]") {
    for (double ratio : {0.8, 1.25}) {
        Run r;
        r.in = sine(1000.0, 44100);
        const uint32_t want = (uint32_t)std::lround(44100 * ratio);
        REQUIRE(r.repitch(want));
        const double f = frequency(r.out, 2000, want - 2000);
        REQUIRE_THAT(f, Catch::Matchers::WithinRel(1000.0 / ratio, 0.01));
    }
}

TEST_CASE("repitch: the clip starts on its first frame and keeps its level", "[dsp][stretch]") {
    Run r;
    r.in = sine(440.0, 44100, 0.5);
    REQUIRE(r.repitch(52000));
    REQUIRE(r.out[0] == r.in[0]);
    REQUIRE(r.out[1] == r.in[1]);
    double sum = 0.0;
    for (uint32_t i = 4000; i < 48000; ++i) sum += (double)r.out[i * 2] * r.out[i * 2];
    const double rms = std::sqrt(sum / 44000.0) / 32767.0;
    REQUIRE_THAT(rms, Catch::Matchers::WithinRel(0.5 / std::sqrt(2.0), 0.02));
}

TEST_CASE("repitch: refuses what it cannot do, and stops when told", "[dsp][stretch]") {
    Run r;
    r.in = sine(220.0, 10000);
    REQUIRE_FALSE(r.repitch(4000));      // ratio 0.4
    REQUIRE_FALSE(r.repitch(25000));     // ratio 2.5
    r.out.clear();
    r.stopAt = 2000;
    REQUIRE_FALSE(r.repitch(12000));
    REQUIRE(r.out.size() < (size_t)12000 * 2);
}

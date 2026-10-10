// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "crosspad/dsp/PitchDetector.hpp"
#include "crosspad/dsp/PortableFft.hpp"

#include <cmath>
#include <vector>

using namespace crosspad;

namespace {

std::vector<int16_t> tone(double hz, uint32_t sr, double seconds, double amp = 0.7) {
    const uint32_t n = static_cast<uint32_t>(sr * seconds);
    std::vector<int16_t> v(n);
    for (uint32_t i = 0; i < n; ++i) {
        v[i] = static_cast<int16_t>(amp * 32000.0 * std::sin(2.0 * M_PI * hz * i / sr));
    }
    return v;
}

double cents(double got, double want) { return 1200.0 * std::log2(got / want); }

} // namespace

TEST_CASE("detectPitch: rejects bad input", "[dsp][pitch]") {
    PortableFft fft;
    PitchResult r;
    REQUIRE_FALSE(detectPitch(fft, nullptr, 100, 44100, r));
    std::vector<int16_t> silence(8192, 0);
    REQUIRE_FALSE(detectPitch(fft, silence.data(), 0, 44100, r));
    REQUIRE_FALSE(detectPitch(fft, silence.data(), 8192, 0, r));
    REQUIRE_FALSE(detectPitch(fft, silence.data(), 8192, 44100, r));
}

TEST_CASE("detectPitch: synthetic sines within 5 cents", "[dsp][pitch]") {
    PortableFft fft;
    for (uint32_t sr : {44100u, 48000u}) {
        for (double hz : {100.0, 155.5, 220.0, 440.0, 997.0, 1500.0, 2000.0}) {
            const auto pcm = tone(hz, sr, 1.0);
            PitchResult r;
            REQUIRE(detectPitch(fft, pcm.data(), (uint32_t)pcm.size(), sr, r));
            const double err = cents(r.hz, hz);
            INFO("sr=" << sr << " hz=" << hz << " got=" << r.hz << " err=" << err << " cents");
            REQUIRE(std::fabs(err) < 5.0);
            REQUIRE(r.confidence > 1.0f);
        }
    }
}

TEST_CASE("detectPitch: A4 lands on MIDI 69", "[dsp][pitch]") {
    PortableFft fft;
    const auto pcm = tone(440.0, 44100, 1.0);
    PitchResult r;
    REQUIRE(detectPitch(fft, pcm.data(), (uint32_t)pcm.size(), 44100, r));
    INFO("midi=" << r.midi);
    REQUIRE(std::fabs(r.midi - 69.0f) < 0.05f);
}

TEST_CASE("detectPitch: picks the fundamental, not an octave", "[dsp][pitch]") {
    PortableFft fft;
    const uint32_t sr = 44100;
    for (double f0 : {110.0, 220.0, 440.0}) {
        const uint32_t n = sr;
        std::vector<int16_t> pcm(n);
        for (uint32_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(i) / sr;
            const double s = std::sin(2.0 * M_PI * f0 * t)
                           + 0.5011872 * std::sin(2.0 * M_PI * 2 * f0 * t)   // -6 dB
                           + 0.2511886 * std::sin(2.0 * M_PI * 3 * f0 * t);  // -12 dB
            pcm[i] = static_cast<int16_t>(18000.0 * s);
        }
        PitchResult r;
        REQUIRE(detectPitch(fft, pcm.data(), n, sr, r));
        INFO("f0=" << f0 << " got=" << r.hz << " (" << cents(r.hz, f0) << " cents)");
        REQUIRE(std::fabs(cents(r.hz, f0)) < 5.0);
    }
}

TEST_CASE("detectPitch: short input is zero-padded", "[dsp][pitch]") {
    PortableFft fft;
    const auto pcm = tone(440.0, 44100, 0.05);   // 2205 samples < kPitchWindow
    PitchResult r;
    REQUIRE(detectPitch(fft, pcm.data(), (uint32_t)pcm.size(), 44100, r));
    INFO("short-frame hz=" << r.hz);
    REQUIRE(std::fabs(cents(r.hz, 440.0)) < 40.0);
}

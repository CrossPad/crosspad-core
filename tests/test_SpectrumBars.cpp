// SPDX-License-Identifier: GPL-3.0-or-later
// Spectrum bars: bar mapping, dB scale, a sine in the right bar.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "crosspad/dsp/SpectrumBars.hpp"
#include "crosspad/dsp/PortableFft.hpp"

#include <cmath>
#include <vector>

using namespace crosspad::dsp;

namespace {

constexpr uint32_t kN = kSpectrumFftSize;
constexpr uint32_t kBars = 50;
constexpr uint32_t kRate = 44100;

/// Levels of a sine of @p amp at @p hz, through the window and an FFT, as a spectrum view does.
std::vector<float> sine_levels(float hz, float amp, uint32_t rate = kRate) {
    std::vector<float> win(kN);
    spectrumHann(win.data(), kN);
    std::vector<float> buf(2 * kN, 0.0f);
    for (uint32_t i = 0; i < kN; ++i) {
        buf[2 * i] = amp * sinf(2.0f * 3.14159265f * hz * static_cast<float>(i) / static_cast<float>(rate))
                     * win[i];
    }
    crosspad::PortableFft fft;
    REQUIRE(fft.forward(buf.data(), kN));
    std::vector<SpectrumBar> map(kBars);
    spectrumMapBars(map.data(), kBars, kN, rate);
    std::vector<float> levels(kBars);
    spectrumLevels(buf.data(), kN, map.data(), kBars, levels.data());
    return levels;
}

uint32_t loudest(const std::vector<float>& v) {
    uint32_t best = 0;
    for (uint32_t i = 1; i < v.size(); ++i) if (v[i] > v[best]) best = i;
    return best;
}

uint32_t bar_of(float hz) {
    uint32_t best = 0;
    float bestDist = 1e9f;
    for (uint32_t i = 0; i < kBars; ++i) {
        const float d = fabsf(logf(spectrumBarHz(i, kBars) / hz));
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

} // namespace

TEST_CASE("bars cover the axis in order, every bar reads valid bins", "[spectrum]") {
    std::vector<SpectrumBar> map(kBars);
    spectrumMapBars(map.data(), kBars, kN, kRate);
    uint32_t prevLo = 0;
    for (uint32_t i = 0; i < kBars; ++i) {
        REQUIRE(map[i].lo <= map[i].hi);
        REQUIRE(map[i].hi < kN / 2);
        REQUIRE(map[i].lo >= prevLo);
        if (map[i].interpolate) {
            REQUIRE(map[i].hi == map[i].lo + 1);
            REQUIRE(map[i].frac >= 0.0f);
            REQUIRE(map[i].frac < 1.0f);
        }
        prevLo = map[i].lo;
    }
    REQUIRE(spectrumBarHz(0, kBars) > kSpectrumLowHz);
    REQUIRE(spectrumBarHz(kBars - 1, kBars) < kSpectrumHighHz);
    REQUIRE(map[0].interpolate);
    REQUIRE_FALSE(map[kBars - 1].interpolate);
}

TEST_CASE("bar frequencies are log spaced", "[spectrum]") {
    const float r0 = spectrumBarHz(1, kBars) / spectrumBarHz(0, kBars);
    const float r1 = spectrumBarHz(40, kBars) / spectrumBarHz(39, kBars);
    REQUIRE(r0 == Catch::Approx(r1).epsilon(1e-4));
}

TEST_CASE("silence draws the floor", "[spectrum]") {
    const auto v = sine_levels(1000.0f, 0.0f);
    for (float x : v) REQUIRE(x == 0.0f);
}

TEST_CASE("a full-scale sine reads about 0 dBFS in its own bar", "[spectrum]") {
    for (float hz : { 110.0f, 440.0f, 1000.0f, 5000.0f, 12000.0f }) {
        const auto v = sine_levels(hz, 1.0f);
        const uint32_t b = loudest(v);
        INFO("hz " << hz << " bar " << b << " expected " << bar_of(hz));
        REQUIRE(std::abs(static_cast<int>(b) - static_cast<int>(bar_of(hz))) <= 1);
        // Hann scalloping and the bar not sitting on the bin cost at most ~3 dB (of 72).
        REQUIRE(v[b] > 1.0f - 3.0f / 72.0f);
        REQUIRE(v[b] <= 1.0f + 1e-3f);
    }
}

TEST_CASE("dB scaling: halving the amplitude drops 6 dB", "[spectrum]") {
    const auto full = sine_levels(1000.0f, 1.0f);
    const auto half = sine_levels(1000.0f, 0.5f);
    const uint32_t b = loudest(full);
    REQUIRE((full[b] - half[b]) * -kSpectrumFloorDb == Catch::Approx(6.02f).margin(0.1));
}

TEST_CASE("a quiet sine below the floor draws nothing", "[spectrum]") {
    const auto v = sine_levels(1000.0f, 1e-5f);   // -100 dBFS
    for (float x : v) REQUIRE(x == 0.0f);
}

TEST_CASE("the sine stands well above the far bars", "[spectrum]") {
    const auto v = sine_levels(1000.0f, 1.0f);
    const uint32_t b = loudest(v);
    for (uint32_t i = 0; i < kBars; ++i) {
        if (std::abs(static_cast<int>(i) - static_cast<int>(b)) > 4) REQUIRE(v[i] < 0.2f);
    }
}

TEST_CASE("a 48 kHz stream lands in the same bar as 44.1 kHz", "[spectrum]") {
    const auto a = sine_levels(2000.0f, 1.0f, 44100);
    const auto b = sine_levels(2000.0f, 1.0f, 48000);
    REQUIRE(std::abs(static_cast<int>(loudest(a)) - static_cast<int>(loudest(b))) <= 1);
}

TEST_CASE("hann window is zero at the start and one in the middle", "[spectrum]") {
    std::vector<float> w(kN);
    spectrumHann(w.data(), kN);
    REQUIRE(w[0] == Catch::Approx(0.0f).margin(1e-6));
    REQUIRE(w[kN / 2] == Catch::Approx(1.0f).margin(1e-6));
}

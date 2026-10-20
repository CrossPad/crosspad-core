// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "crosspad/sampletools/SampleFx.hpp"

#include <cmath>
#include <cstdlib>
#include <vector>

using namespace crosspad;

namespace {

constexpr uint32_t kRate = 44100;

/// Peak of a sine at @p hz after @p fx, once the filters have settled, as a
/// fraction of what went in.
double gain(const SampleFx& fx, double hz)
{
    const uint32_t n = kRate / 2;
    std::vector<int16_t> io(n * 2);
    for (uint32_t i = 0; i < n; ++i) {
        const auto v = (int16_t)std::lround(16000.0 * std::sin(2.0 * 3.14159265358979 * hz * i / kRate));
        io[i * 2] = io[i * 2 + 1] = v;
    }
    SampleFilters f;
    f.set(fx, kRate);
    f.process(io.data(), n);
    int peak = 0;
    for (uint32_t i = n / 2; i < n; ++i) peak = std::max(peak, std::abs((int)io[i * 2]));
    return peak / 16000.0;
}

} // namespace

TEST_CASE("sample fx: no filter passes everything untouched", "[samplefx]") {
    CHECK(gain(SampleFx{}, 1000) > 0.999);
}

TEST_CASE("sample fx: a low pass keeps the lows and cuts the highs, 12 dB an octave", "[samplefx]") {
    const SampleFx lp{0, 0, 1000};
    CHECK(gain(lp, 100) > 0.98);
    CHECK(std::fabs(gain(lp, 1000) - 0.707) < 0.03);   // -3 dB at the cutoff
    CHECK(gain(lp, 4000) < 0.08);                       // two octaves: about -24 dB
}

TEST_CASE("sample fx: a high pass keeps the highs and cuts the lows", "[samplefx]") {
    const SampleFx hp{0, 1000, 0};
    CHECK(gain(hp, 8000) > 0.98);
    CHECK(std::fabs(gain(hp, 1000) - 0.707) < 0.03);
    CHECK(gain(hp, 250) < 0.08);
}

TEST_CASE("sample fx: a cutoff past the band is off", "[samplefx]") {
    Biquad b;
    b.set(Biquad::Kind::LowPass, 30000, kRate);
    CHECK_FALSE(b.on());
}

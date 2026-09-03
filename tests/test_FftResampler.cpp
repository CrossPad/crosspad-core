// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include "crosspad/dsp/FftResampler.hpp"
#include "crosspad/dsp/PitchDetector.hpp"
#include "crosspad/dsp/PortableFft.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

using namespace crosspad;

namespace {

void* testAlloc(size_t n) { return std::malloc(n); }
void  testFree(void* p)   { std::free(p); }

std::vector<int16_t> tone(double hz, uint32_t sr, double seconds, double amp) {
    const uint32_t n = static_cast<uint32_t>(sr * seconds);
    std::vector<int16_t> v(n);
    for (uint32_t i = 0; i < n; ++i) {
        v[i] = static_cast<int16_t>(amp * std::sin(2.0 * M_PI * hz * i / sr));
    }
    return v;
}

/// Frequency from zero crossings, linearly interpolated, over [from, to).
double zeroCrossingHz(const std::vector<int16_t>& v, uint32_t from, uint32_t to, uint32_t sr) {
    double first = -1.0, last = -1.0;
    int count = 0;
    for (uint32_t i = from + 1; i < to; ++i) {
        if (v[i - 1] <= 0 && v[i] > 0) {
            const double d = static_cast<double>(-v[i - 1]) / (v[i] - v[i - 1]);
            const double pos = (i - 1) + d;
            if (first < 0.0) first = pos;
            else { last = pos; ++count; }
        }
    }
    if (count < 2 || last < 0.0) return 0.0;
    return count * static_cast<double>(sr) / (last - first);
}

double rms(const std::vector<int16_t>& v, uint32_t from, uint32_t to) {
    double s = 0.0;
    for (uint32_t i = from; i < to; ++i) s += static_cast<double>(v[i]) * v[i];
    return std::sqrt(s / (to - from));
}

} // namespace

TEST_CASE("resampledLength: rounds to the nearest frame", "[dsp][resample]") {
    REQUIRE(resampledLength(48000, 48000, 44100) == 44100);
    REQUIRE(resampledLength(24000, 48000, 44100) == 22050);
    REQUIRE(resampledLength(1000, 44100, 44100) == 1000);
    REQUIRE(resampledLength(1000, 22050, 44100) == 2000);
    REQUIRE(resampledLength(0, 44100, 48000) == 0);
    REQUIRE(resampledLength(100, 0, 48000) == 0);
}

TEST_CASE("fftResample: 48000 -> 44100 keeps the tone", "[dsp][resample]") {
    PortableFft fft;
    const double hz = 997.0;
    const double amp = 16000.0;
    const auto in = tone(hz, 48000, 0.5, amp);
    const uint32_t outCount = resampledLength((uint32_t)in.size(), 48000, 44100);
    REQUIRE(outCount == 22050);

    std::vector<int16_t> out(outCount, 0);
    REQUIRE(fftResample(fft, in.data(), (uint32_t)in.size(), 48000,
                        out.data(), outCount, 44100, testAlloc, testFree));

    const double measured = zeroCrossingHz(out, 3000, outCount - 3000, 44100);
    INFO("zero-crossing Hz = " << measured);
    REQUIRE(std::fabs(measured - hz) < 0.2);

    PitchResult r;
    REQUIRE(detectPitch(fft, out.data(), outCount, 44100, r));
    INFO("detector Hz = " << r.hz);
    REQUIRE(std::fabs(r.hz - hz) < 0.5);

    const double gotRms = rms(out, 3000, outCount - 3000);
    const double wantRms = amp / std::sqrt(2.0);
    INFO("rms " << gotRms << " want " << wantRms);
    REQUIRE(std::fabs(gotRms - wantRms) / wantRms < 0.01);
}

TEST_CASE("fftResample: same rate is a copy", "[dsp][resample]") {
    PortableFft fft;
    const auto in = tone(440.0, 44100, 0.25, 20000.0);
    const uint32_t outCount = resampledLength((uint32_t)in.size(), 44100, 44100);
    REQUIRE(outCount == in.size());

    std::vector<int16_t> out(outCount, 0);
    REQUIRE(fftResample(fft, in.data(), (uint32_t)in.size(), 44100,
                        out.data(), outCount, 44100, testAlloc, testFree));

    int worst = 0;
    for (uint32_t i = 0; i < outCount; ++i) {
        worst = std::max(worst, std::abs(static_cast<int>(out[i]) - static_cast<int>(in[i])));
    }
    INFO("worst identity deviation = " << worst << " LSB");
    REQUIRE(worst <= 1);
}

TEST_CASE("fftResample: rejects bad arguments", "[dsp][resample]") {
    PortableFft fft;
    std::vector<int16_t> in(1000, 0), out(1000, 0);
    REQUIRE_FALSE(fftResample(fft, nullptr, 1000, 48000, out.data(), 1000, 44100, testAlloc, testFree));
    REQUIRE_FALSE(fftResample(fft, in.data(), 1000, 48000, nullptr, 1000, 44100, testAlloc, testFree));
    REQUIRE_FALSE(fftResample(fft, in.data(), 0, 48000, out.data(), 1000, 44100, testAlloc, testFree));
    REQUIRE_FALSE(fftResample(fft, in.data(), 1000, 0, out.data(), 1000, 44100, testAlloc, testFree));
    REQUIRE_FALSE(fftResample(fft, in.data(), 1000, 48000, out.data(), 1000, 44100, nullptr, testFree));
}

TEST_CASE("fftResample: upsampling keeps the tone", "[dsp][resample]") {
    PortableFft fft;
    const auto in = tone(440.0, 22050, 0.4, 15000.0);
    const uint32_t outCount = resampledLength((uint32_t)in.size(), 22050, 44100);
    std::vector<int16_t> out(outCount, 0);
    REQUIRE(fftResample(fft, in.data(), (uint32_t)in.size(), 22050,
                        out.data(), outCount, 44100, testAlloc, testFree));
    const double measured = zeroCrossingHz(out, 4000, outCount - 4000, 44100);
    INFO("upsampled Hz = " << measured);
    REQUIRE(std::fabs(measured - 440.0) < 0.2);
}

TEST_CASE("fftResample: a 4096-point backend matches the 8192 one", "[dsp][resample]") {
    // esp-dsp on a prebuilt Arduino sdkconfig tops out at 4096, which halves
    // the analysis block (the inverse runs at 2N). The conversion has to be
    // the same conversion: same length, same tone, same amplitude — only more
    // overlap-add passes and less scratch.
    PortableFft full;
    PortableFft capped(4096);
    REQUIRE(full.maxSize() == 8192);
    REQUIRE(capped.maxSize() == 4096);

    const double hz = 997.0;
    const double amp = 16000.0;
    const auto in = tone(hz, 48000, 0.5, amp);
    const uint32_t outCount = resampledLength((uint32_t)in.size(), 48000, 44100);
    REQUIRE(outCount == 22050);

    std::vector<int16_t> a(outCount, 0), b(outCount, 0);
    REQUIRE(fftResample(full, in.data(), (uint32_t)in.size(), 48000,
                        a.data(), outCount, 44100, testAlloc, testFree));
    REQUIRE(fftResample(capped, in.data(), (uint32_t)in.size(), 48000,
                        b.data(), outCount, 44100, testAlloc, testFree));

    const double hzA = zeroCrossingHz(a, 3000, outCount - 3000, 44100);
    const double hzB = zeroCrossingHz(b, 3000, outCount - 3000, 44100);
    INFO("8192 backend " << hzA << " Hz, 4096 backend " << hzB << " Hz");
    REQUIRE(std::fabs(hzB - hz) < 0.2);
    REQUIRE(std::fabs(hzB - hzA) < 0.05);

    const double rmsB = rms(b, 3000, outCount - 3000);
    REQUIRE(std::fabs(rmsB - amp / std::sqrt(2.0)) / (amp / std::sqrt(2.0)) < 0.01);

    int worst = 0;
    for (uint32_t i = 3000; i < outCount - 3000; ++i) {
        worst = std::max(worst, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
    }
    INFO("worst sample deviation between the two blocks = " << worst << " LSB");
    REQUIRE(worst <= 8);
}

TEST_CASE("fftResample: refuses a backend too small to hold a block", "[dsp][resample]") {
    PortableFft tiny(64);   // 64 / 2 == 32, below kMinFftSize
    const auto in = tone(440.0, 48000, 0.05, 10000.0);
    const uint32_t outCount = resampledLength((uint32_t)in.size(), 48000, 44100);
    std::vector<int16_t> out(outCount, 0);
    REQUIRE_FALSE(fftResample(tiny, in.data(), (uint32_t)in.size(), 48000,
                              out.data(), outCount, 44100, testAlloc, testFree));
}

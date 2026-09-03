// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include "crosspad/dsp/PortableFft.hpp"

#include <cmath>
#include <random>
#include <vector>

using namespace crosspad;

namespace {

/// Reference O(n^2) DFT on interleaved [re,im] data.
std::vector<float> naiveDft(const std::vector<float>& in, uint32_t n) {
    std::vector<float> out(2 * n, 0.0f);
    for (uint32_t k = 0; k < n; ++k) {
        double sr = 0.0, si = 0.0;
        for (uint32_t t = 0; t < n; ++t) {
            const double a = -2.0 * M_PI * static_cast<double>(k) * t / n;
            const double c = std::cos(a), s = std::sin(a);
            sr += in[2 * t] * c - in[2 * t + 1] * s;
            si += in[2 * t] * s + in[2 * t + 1] * c;
        }
        out[2 * k]     = static_cast<float>(sr);
        out[2 * k + 1] = static_cast<float>(si);
    }
    return out;
}

std::vector<float> randomComplex(uint32_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);
    std::vector<float> v(2 * n);
    for (auto& x : v) x = d(rng);
    return v;
}

} // namespace

TEST_CASE("PortableFft: size validation", "[dsp][fft]") {
    PortableFft fft;
    std::vector<float> buf(2 * 128, 0.0f);
    REQUIRE(fft.forward(buf.data(), 128));
    REQUIRE_FALSE(fft.forward(buf.data(), 100));   // not a power of two
    REQUIRE_FALSE(fft.forward(buf.data(), 32));    // below kMinFftSize
    REQUIRE_FALSE(fft.forward(buf.data(), 16384)); // above kMaxFftSize
    REQUIRE_FALSE(fft.forward(nullptr, 128));
}

TEST_CASE("PortableFft: matches a naive DFT", "[dsp][fft]") {
    PortableFft fft;
    for (uint32_t n : {64u, 1024u}) {
        const std::vector<float> in = randomComplex(n, 1234u + n);
        const std::vector<float> ref = naiveDft(in, n);

        std::vector<float> got = in;
        REQUIRE(fft.forward(got.data(), n));

        // Relative to the transform's own magnitude scale (sqrt(n) for noise).
        double norm = 0.0;
        for (uint32_t k = 0; k < 2 * n; ++k) norm = std::max(norm, std::fabs((double)ref[k]));
        REQUIRE(norm > 0.0);
        for (uint32_t k = 0; k < 2 * n; ++k) {
            REQUIRE(std::fabs(got[k] - ref[k]) / norm < 1e-3);
        }
    }
}

TEST_CASE("PortableFft: forward then inverse is the identity", "[dsp][fft]") {
    PortableFft fft;
    for (uint32_t n : {64u, 256u, 4096u}) {
        const std::vector<float> in = randomComplex(n, 77u + n);
        std::vector<float> buf = in;
        REQUIRE(fft.forward(buf.data(), n));
        REQUIRE(fft.inverse(buf.data(), n));
        for (uint32_t k = 0; k < 2 * n; ++k) {
            REQUIRE(std::fabs(buf[k] - in[k]) < 1e-4f);
        }
    }
}

TEST_CASE("PortableFft: a pure bin lands in one bin", "[dsp][fft]") {
    PortableFft fft;
    const uint32_t n = 512;
    const uint32_t bin = 37;
    std::vector<float> buf(2 * n, 0.0f);
    for (uint32_t t = 0; t < n; ++t) {
        const double a = 2.0 * M_PI * bin * t / n;
        buf[2 * t]     = static_cast<float>(std::cos(a));
        buf[2 * t + 1] = static_cast<float>(std::sin(a));
    }
    REQUIRE(fft.forward(buf.data(), n));
    for (uint32_t k = 0; k < n; ++k) {
        const float mag = std::hypot(buf[2 * k], buf[2 * k + 1]);
        if (k == bin) REQUIRE(std::fabs(mag - static_cast<float>(n)) < 0.05f * n);
        else          REQUIRE(mag < 1e-2f * n);
    }
}

TEST_CASE("PortableFft: maxSize is kMaxFftSize and caps transforms", "[dsp][fft]") {
    PortableFft full;
    REQUIRE(full.maxSize() == kMaxFftSize);

    PortableFft capped(4096);
    REQUIRE(capped.maxSize() == 4096);
    std::vector<float> buf(2 * kMaxFftSize, 0.0f);
    REQUIRE(capped.forward(buf.data(), 4096));
    REQUIRE_FALSE(capped.forward(buf.data(), 8192));
    REQUIRE_FALSE(capped.inverse(buf.data(), 8192));

    // Not a power of two, and below the floor: rounded into range, never
    // left at a size no transform accepts.
    REQUIRE(PortableFft(5000).maxSize() == 4096);
    REQUIRE(PortableFft(0).maxSize() == kMinFftSize);
    REQUIRE(PortableFft(99999).maxSize() == kMaxFftSize);
}

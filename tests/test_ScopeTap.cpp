// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "crosspad/audio/ScopeTap.hpp"

#include <vector>

using namespace crosspad;

namespace {

/// A block whose sample k holds `first + k`, so order is readable at a glance.
std::vector<float> ramp(uint32_t first, uint32_t n) {
    std::vector<float> v(n);
    for (uint32_t i = 0; i < n; ++i) v[i] = static_cast<float>(first + i);
    return v;
}

} // namespace

TEST_CASE("ScopeTap: empty tap copies nothing", "[audio][scope]") {
    ScopeTap tap;
    std::vector<float> out(16, -1.0f);
    REQUIRE(tap.snapshot(out.data(), 16) == 0u);
    REQUIRE(out[0] == -1.0f);           // untouched
    REQUIRE(tap.written() == 0u);
}

TEST_CASE("ScopeTap: disabled producer does no capture work", "[audio][scope]") {
    ScopeTap tap;
    const float in[] = {0.25f, -0.5f, 0.75f};
    float out[3]{};

    tap.setEnabled(false);
    tap.write(in, 3);
    REQUIRE(tap.written() == 0);
    REQUIRE(tap.snapshot(out, 3) == 0);

    tap.setEnabled(true);
    tap.write(in, 3);
    REQUIRE(tap.snapshot(out, 3) == 3);
    REQUIRE(out[0] == in[0]);
    REQUIRE(out[1] == in[1]);
    REQUIRE(out[2] == in[2]);
}

TEST_CASE("ScopeTap: a partial fill answers with what it has", "[audio][scope]") {
    ScopeTap tap;
    const std::vector<float> in = ramp(0, 100);
    tap.write(in.data(), 100);

    std::vector<float> out(512, -1.0f);
    REQUIRE(tap.snapshot(out.data(), 512) == 100u);
    for (uint32_t i = 0; i < 100; ++i) REQUIRE(out[i] == static_cast<float>(i));
    REQUIRE(out[100] == -1.0f);         // nothing written past the count

    // A smaller request is the newest tail, still oldest first.
    REQUIRE(tap.snapshot(out.data(), 10) == 10u);
    for (uint32_t i = 0; i < 10; ++i) REQUIRE(out[i] == static_cast<float>(90 + i));
}

TEST_CASE("ScopeTap: consecutive blocks read back as one stream", "[audio][scope]") {
    ScopeTap tap;
    for (uint32_t b = 0; b < 10; ++b) {
        const std::vector<float> in = ramp(b * 64, 64);
        tap.write(in.data(), 64);
    }
    REQUIRE(tap.written() == 640u);

    std::vector<float> out(640);
    REQUIRE(tap.snapshot(out.data(), 640) == 640u);
    for (uint32_t i = 0; i < 640; ++i) REQUIRE(out[i] == static_cast<float>(i));
}

TEST_CASE("ScopeTap: the ring wraps and keeps the newest samples", "[audio][scope]") {
    ScopeTap tap;
    // One and a half turns of the ring, in blocks that do not divide it evenly
    // so a wrap lands mid-block.
    const uint32_t blocks = (ScopeTap::kCapacity * 3) / (2 * 100) + 1;
    for (uint32_t b = 0; b < blocks; ++b) {
        const std::vector<float> in = ramp(b * 100, 100);
        tap.write(in.data(), 100);
    }
    const uint32_t total = blocks * 100;
    REQUIRE(total > ScopeTap::kCapacity);
    REQUIRE(tap.written() == total);

    std::vector<float> out(ScopeTap::kCapacity);
    REQUIRE(tap.snapshot(out.data(), ScopeTap::kCapacity) == ScopeTap::kCapacity);
    const uint32_t oldest = total - ScopeTap::kCapacity;
    for (uint32_t i = 0; i < ScopeTap::kCapacity; ++i) {
        REQUIRE(out[i] == static_cast<float>(oldest + i));
    }
}

TEST_CASE("ScopeTap: a request longer than the ring is capped", "[audio][scope]") {
    ScopeTap tap;
    const std::vector<float> in = ramp(0, ScopeTap::kCapacity);
    tap.write(in.data(), ScopeTap::kCapacity);

    std::vector<float> out(ScopeTap::kCapacity + 64, -1.0f);
    REQUIRE(tap.snapshot(out.data(), ScopeTap::kCapacity + 64) == ScopeTap::kCapacity);
    REQUIRE(out[ScopeTap::kCapacity] == -1.0f);
}

TEST_CASE("ScopeTap: an oversized block keeps only its newest tail", "[audio][scope]") {
    ScopeTap tap;
    const uint32_t n = ScopeTap::kCapacity + 500;
    const std::vector<float> in = ramp(0, n);
    tap.write(in.data(), n);

    REQUIRE(tap.written() == n);        // the count stays honest
    std::vector<float> out(ScopeTap::kCapacity);
    REQUIRE(tap.snapshot(out.data(), ScopeTap::kCapacity) == ScopeTap::kCapacity);
    for (uint32_t i = 0; i < ScopeTap::kCapacity; ++i) {
        REQUIRE(out[i] == static_cast<float>(500 + i));
    }
}

TEST_CASE("ScopeTap: degenerate arguments are refused, not crashed", "[audio][scope]") {
    ScopeTap tap;
    std::vector<float> out(8, -1.0f);
    tap.write(nullptr, 8);
    const std::vector<float> in = ramp(0, 8);
    tap.write(in.data(), 0);
    REQUIRE(tap.written() == 0u);
    REQUIRE(tap.snapshot(nullptr, 8) == 0u);
    REQUIRE(tap.snapshot(out.data(), 0) == 0u);
}

TEST_CASE("ScopeTap: the owner sets the sample rate", "[audio][scope]") {
    ScopeTap tap;
    REQUIRE(tap.sampleRate() == 0u);
    tap.setSampleRate(44100);
    REQUIRE(tap.sampleRate() == 44100u);

    const std::vector<float> in = ramp(0, 32);
    tap.write(in.data(), 32);
    tap.clear();
    std::vector<float> out(32);
    REQUIRE(tap.snapshot(out.data(), 32) == 0u);
    REQUIRE(tap.sampleRate() == 44100u);   // clearing history keeps the rate
}

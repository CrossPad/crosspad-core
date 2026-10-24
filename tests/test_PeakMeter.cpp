// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "crosspad/audio/PeakMeter.hpp"

using Catch::Matchers::WithinAbs;
using namespace crosspad;

TEST_CASE("PeakMeter: starts at zero", "[audio][meter]") {
    PeakMeter m;
    REQUIRE(m.left()  == 0);
    REQUIRE(m.right() == 0);
    REQUIRE(m.leftFloat()  == 0.0f);
    REQUIRE(m.rightFloat() == 0.0f);
}

TEST_CASE("PeakMeter: int update captures raw peak", "[audio][meter]") {
    PeakMeter m(230);  // ~0.90 decay
    m.update(16384, 8192);
    // First update: decay (0 * 0.9 = 0) then rawL > peakL → peakL = rawL
    REQUIRE(m.left()  == 16384);
    REQUIRE(m.right() == 8192);
}

TEST_CASE("PeakMeter: monotonic decay tail", "[audio][meter]") {
    PeakMeter m(230);
    m.update(16384, 16384);
    int16_t prev = m.left();
    REQUIRE(prev == 16384);

    for (int i = 0; i < 100; ++i) {
        m.update(0, 0);
        const int16_t cur = m.left();
        REQUIRE(cur <= prev);   // monotonic decrease
        REQUIRE(cur >= 0);      // never negative
        prev = cur;
    }
    // After 100 frames at 0.9 decay, peak should approach 0
    REQUIRE(m.left()  <= 1);
    REQUIRE(m.right() <= 1);
}

TEST_CASE("PeakMeter: float overload normalizes to int16", "[audio][meter]") {
    PeakMeter m(230);
    m.updateFloat(0.5f, 0.25f);

    // 0.5 * 32767 = 16383
    REQUIRE(m.left()  == 16383);
    REQUIRE(m.right() == 8191);
    REQUIRE_THAT(m.leftFloat(),  WithinAbs(0.5f,  1.0f / 32767.0f));
    REQUIRE_THAT(m.rightFloat(), WithinAbs(0.25f, 1.0f / 32767.0f));
}

TEST_CASE("PeakMeter: float overload clamps inputs", "[audio][meter]") {
    PeakMeter m;
    m.updateFloat(1.5f, -1.5f);  // exceeds [-1, 1]
    // Both clamp to magnitude 1.0 → int16 32767
    REQUIRE(m.left()  == 32767);
    REQUIRE(m.right() == 32767);
}

TEST_CASE("PeakMeter: reset clears state", "[audio][meter]") {
    PeakMeter m;
    m.update(16384, 16384);
    REQUIRE(m.left()  != 0);

    m.reset();
    REQUIRE(m.left()  == 0);
    REQUIRE(m.right() == 0);
}

TEST_CASE("PeakMeter: hold-and-decay pattern", "[audio][meter]") {
    PeakMeter m(238);  // 238/256 ≈ 0.93 (matches AbstractAudioModule kDecay)
    m.update(20000, 20000);
    REQUIRE(m.left() == 20000);

    // Smaller subsequent peak is masked by held value
    m.update(5000, 5000);
    REQUIRE(m.left()  > 5000);   // decayed peak still above raw
    REQUIRE(m.right() > 5000);

    // Larger peak overrides
    m.update(30000, 30000);
    REQUIRE(m.left()  == 30000);
    REQUIRE(m.right() == 30000);
}

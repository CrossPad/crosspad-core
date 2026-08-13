#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"

TEST_CASE("PadAnimator: inactive charging is a refreshed static gauge", "[pad][animator]") {
    test::MockClock clock;
    test::MockLedStrip strip;
    crosspad::PadAnimator animator;
    animator.init(strip, clock);

    animator.startCharging(0.5f);
    animator.setChargingActive(false);
    animator.update();

    const crosspad::RgbColor expected(127, 127, 0);
    for (int i = 0; i < 8; ++i) {
        REQUIRE(strip.pixels[i] == expected);
    }
    for (int i = 8; i < 16; ++i) {
        REQUIRE(strip.pixels[i] == crosspad::RgbColor(0, 0, 0));
    }
    REQUIRE(strip.refreshCount == 1);

    clock.advance(5'000'000);
    animator.update();
    for (int i = 0; i < 8; ++i) {
        REQUIRE(strip.pixels[i] == expected);
    }
    REQUIRE(strip.refreshCount == 2);
}

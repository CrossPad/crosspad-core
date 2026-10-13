// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "crosspad/stm32/VelocityCurve.hpp"

using namespace crosspad;

TEST_CASE("stm32VelCurveApply linear is identity above floor", "[stm][curve]") {
    REQUIRE(stm32VelCurveApply(64, STM32_VEL_LINEAR, 1) == 64);
    REQUIRE(stm32VelCurveApply(127, STM32_VEL_LINEAR, 1) == 127);
}

TEST_CASE("stm32VelCurveApply hard is concave (v^2/127)", "[stm][curve]") {
    REQUIRE(stm32VelCurveApply(64, STM32_VEL_EXP_HARD, 1) == (uint8_t)(64u*64u/127u)); // 32
    REQUIRE(stm32VelCurveApply(127, STM32_VEL_EXP_HARD, 1) == 127);
}

TEST_CASE("stm32VelCurveApply soft is convex", "[stm][curve]") {
    // 127 - (127-64)^2/127 = 127 - 63*63/127 = 127 - 31 = 96
    REQUIRE(stm32VelCurveApply(64, STM32_VEL_EXP_SOFT, 1) == 96);
}

TEST_CASE("stm32VelCurveApply applies floor and never returns 0 while pressed", "[stm][curve]") {
    REQUIRE(stm32VelCurveApply(0, STM32_VEL_LINEAR, 1) == 1);      // min-1 clamp
    REQUIRE(stm32VelCurveApply(10, STM32_VEL_LINEAR, 40) == 40);   // floor raises
    REQUIRE(stm32VelCurveApply(100, STM32_VEL_LINEAR, 40) == 100); // above floor untouched
}

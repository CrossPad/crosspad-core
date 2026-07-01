#include <catch2/catch_test_macros.hpp>
#include "crosspad/config/crosspad_config.h"

TEST_CASE("config: board profile defaults match baseline", "[config]") {
    REQUIRE(BOARD_PAD_SENS_DEF        == 1);
    REQUIRE(BOARD_PAD_LR_RATIO_DEF    == 50);
    REQUIRE(BOARD_PAD_ANALOG_MODE_DEF == 2);
    REQUIRE(BOARD_VEL_CURVE_DEF       == 0);
    REQUIRE(BOARD_VEL_FLOOR_DEF       == 1);
    REQUIRE(BOARD_CC_PRESS_BASE_DEF   == 16);
    REQUIRE(BOARD_CC_PAN_BASE_DEF     == 48);
    REQUIRE(BOARD_ANALOG_MIN_MS_DEF   == 16);
    REQUIRE(BOARD_ANALOG_DELTA_DEF    == 2);
    REQUIRE(BOARD_LCD_BRIGHTNESS_DEF  == 100);
    REQUIRE(BOARD_RGB_BRIGHTNESS_DEF  == 100);
}

TEST_CASE("config: default pad-config level is FULL", "[config]") {
    REQUIRE(CROSSPAD_PAD_CONFIG_LEVEL == CROSSPAD_PAD_CONFIG_FULL);
#if defined(PAD_CONFIG_PER_PAD)
    SUCCEED("PAD_CONFIG_PER_PAD derived from FULL level");
#else
    FAIL("PAD_CONFIG_PER_PAD should be defined at FULL level");
#endif
}

TEST_CASE("config: PC profile selected when no board -D given", "[config]") {
    REQUIRE(CROSSPAD_BOARD == BOARD_CROSSPAD_PC);
}

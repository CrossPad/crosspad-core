#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"
#include "crosspad/settings/Stm32Settings.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/config/crosspad_config.h"

using namespace crosspad;

TEST_CASE("Stm32Settings: defaults match STM compiled defaults", "[settings][stm]") {
    Stm32Settings s;
    REQUIRE(s.audioEn == 0);
    REQUIRE(s.autoSleep == 1);
    REQUIRE(s.midiChannel == 0);
    REQUIRE(s.noteOffset == 0);
    REQUIRE(s.keypadEnabled == true);
    REQUIRE(s.dimTimeoutS == 1000);
    REQUIRE(s.sleepTimeoutS == 1800);
    REQUIRE(s.longPressMs == 2000);
    REQUIRE(s.wakeBrightness == 50);
    REQUIRE(s.ccPressBase == 16);
    REQUIRE(s.ccPanBase == 48);
    REQUIRE(s.analogMinMs == 16);
    REQUIRE(s.analogDelta == 2);
    REQUIRE(s.en3v3Hold == 1);
    REQUIRE(s.usbSuspendS == 30);
    REQUIRE(s.encCcMode == 0);
    REQUIRE(s.encCcNum == 1);
    REQUIRE(s.encCcChan == 0);
    REQUIRE(s.encCcStep == 1);
    REQUIRE(s.velCurveDef == 0);
    REQUIRE(s.panCcEn == 0);
#if defined(PAD_CONFIG_PER_PAD)
    for (int i = 0; i < 16; ++i) {
        REQUIRE(s.padAnalogMode[i] == 2);     // ANALOG_AFTER_TOUCH
        REQUIRE(s.padSensitivity[i] == 1);
        REQUIRE(s.padLrRatio[i] == 50);
        REQUIRE(s.padVelCurve[i] == 0xFF);    // inherit
        REQUIRE(s.padVelFloor[i] == 0xFF);    // inherit
    }
#endif
}

TEST_CASE("Stm32Settings: persistence round-trip", "[settings][stm]") {
    test::MemoryKVStore kv;
    Stm32Settings a;
    a.midiChannel = 5;
    a.noteOffset = -12;
    a.sleepTimeoutS = 900;
#if defined(PAD_CONFIG_PER_PAD)
    a.padAnalogMode[3] = 1;
    a.padSensitivity[3] = 250;
#endif
    a.save(kv);

    Stm32Settings b;
    b.load(kv);
    REQUIRE(b.midiChannel == 5);
    REQUIRE(b.noteOffset == -12);
    REQUIRE(b.sleepTimeoutS == 900);
#if defined(PAD_CONFIG_PER_PAD)
    REQUIRE(b.padAnalogMode[3] == 1);
    REQUIRE(b.padSensitivity[3] == 250);
#endif
}

TEST_CASE("Stm32Settings: analog defaults sourced from board profile", "[settings][stm][config]") {
    Stm32Settings s;
    REQUIRE(s.velCurveDef == BOARD_VEL_CURVE_DEF);
    REQUIRE(s.velFloorDef == BOARD_VEL_FLOOR_DEF);
    REQUIRE(s.ccPressBase == BOARD_CC_PRESS_BASE_DEF);
    REQUIRE(s.ccPanBase   == BOARD_CC_PAN_BASE_DEF);
    REQUIRE(s.analogMinMs == BOARD_ANALOG_MIN_MS_DEF);
    REQUIRE(s.analogDelta == BOARD_ANALOG_DELTA_DEF);
}

TEST_CASE("Stm32Settings: accessors alias per-pad storage", "[settings][stm][accessor]") {
    Stm32Settings s;
#if defined(PAD_CONFIG_PER_PAD)
    s.sensitivity(3) = 200; s.lrRatio(3) = 40; s.analogMode(3) = 1;
    s.velCurve(3) = 2; s.velFloor(3) = 7;
    const Stm32Settings& cs = s;
    REQUIRE(cs.sensitivity(3) == 200);
    REQUIRE(cs.lrRatio(3)     == 40);
    REQUIRE(cs.analogMode(3)  == 1);
    REQUIRE(cs.velCurve(3)    == 2);
    REQUIRE(cs.velFloor(3)    == 7);
    REQUIRE(cs.sensitivity(0) == BOARD_PAD_SENS_DEF);
#else
    s.sensitivity(3) = 200;
    const Stm32Settings& cs = s;
    REQUIRE(cs.sensitivity(0) == 200);   // one global value, all indices alias it
    REQUIRE(cs.sensitivity(15) == 200);
#endif
}

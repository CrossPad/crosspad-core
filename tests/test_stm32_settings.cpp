#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"
#include "crosspad/settings/Stm32Settings.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"

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
    for (int i = 0; i < 16; ++i) {
        REQUIRE(s.padAnalogMode[i] == 2);     // ANALOG_AFTER_TOUCH
        REQUIRE(s.padSensitivity[i] == 1);
        REQUIRE(s.padLrRatio[i] == 50);
        REQUIRE(s.padVelCurve[i] == 0xFF);    // inherit
        REQUIRE(s.padVelFloor[i] == 0xFF);    // inherit
    }
}

TEST_CASE("Stm32Settings: persistence round-trip", "[settings][stm]") {
    test::MemoryKVStore kv;
    Stm32Settings a;
    a.midiChannel = 5;
    a.noteOffset = -12;
    a.sleepTimeoutS = 900;
    a.padAnalogMode[3] = 1;
    a.padSensitivity[3] = 250;
    a.save(kv);

    Stm32Settings b;
    b.load(kv);
    REQUIRE(b.midiChannel == 5);
    REQUIRE(b.noteOffset == -12);
    REQUIRE(b.sleepTimeoutS == 900);
    REQUIRE(b.padAnalogMode[3] == 1);
    REQUIRE(b.padSensitivity[3] == 250);
}

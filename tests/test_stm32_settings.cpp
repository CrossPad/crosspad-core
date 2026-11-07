// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"
#include "crosspad/settings/Stm32Settings.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/config/crosspad_config.h"

using namespace crosspad;

TEST_CASE("Stm32Settings: defaults match STM compiled defaults", "[settings][stm]") {
    Stm32Settings s;
    // audioEn defaults ON and mirrors AudioSettings::speakersEnabled; it used to
    // be 0, which muted the amp on every config push.
    REQUIRE(s.audioEn == 1);
    REQUIRE(s.autoSleep == 1);
    REQUIRE(s.midiChannel == 0);
    REQUIRE(s.noteOffset == 0);
    REQUIRE(s.keypadEnabled == true);
    REQUIRE(s.dimTimeoutS == 600);   // matches a preset the settings UI can show
    REQUIRE(s.sleepTimeoutS == 1800);
    REQUIRE(s.longPressMs == 2400);
    REQUIRE(s.wakeBrightness == 50);
    REQUIRE(s.ccPressBase == 16);
    REQUIRE(s.ccPanBase == 48);
    REQUIRE(s.analogMinMs == 16);
    REQUIRE(s.analogDelta == 2);
    REQUIRE(s.pressDebounceMs == 6);  // not the STM's 15: the note-on waits it out
    REQUIRE(s.hitMode == 0);          // fixed window until the A/B picks
    REQUIRE(s.hitMinMs == 2);
    REQUIRE(s.hitPeakDrop == 32);
    REQUIRE(s.hitPeakSamples == 2);
    REQUIRE(s.en3v3Hold == 1);
    REQUIRE(s.usbSuspendS == 30);
    REQUIRE(s.encCcMode == 0);
    REQUIRE(s.encCcNum == 1);
    REQUIRE(s.encCcChan == 0);
    REQUIRE(s.encCcStep == 1);
    REQUIRE(s.velCurveDef == 0);
    REQUIRE(s.panCcEn == 1);         // pan reaches pad logic since core 1.17.0 (onPadPan)
    REQUIRE(s.padPanSwap == (1u << 6));  // pad 6 reports its pan inverted (1.17.1)
#if defined(PAD_CONFIG_PER_PAD)
    for (int i = 0; i < 16; ++i) {
        REQUIRE(s.padAnalogMode[i] == 2);     // ANALOG_AFTER_TOUCH
        REQUIRE(s.padSensitivity[i] == 1);
        REQUIRE(s.padLrRatio[i] == 50);
        REQUIRE(s.padVelCurve[i] == 0xFF);    // inherit
        REQUIRE(s.padVelFloor[i] == 0xFF);    // inherit
    }
#else
    REQUIRE(s.sensitivity(0)  == BOARD_PAD_SENS_DEF);
    REQUIRE(s.lrRatio(0)      == BOARD_PAD_LR_RATIO_DEF);
    REQUIRE(s.analogMode(0)   == BOARD_PAD_ANALOG_MODE_DEF);
    REQUIRE(s.velCurve(0)     == 0xFF);
    REQUIRE(s.velFloor(0)     == 0xFF);
#endif
}

TEST_CASE("Stm32Settings: persistence round-trip", "[settings][stm]") {
    test::MemoryKVStore kv;
    Stm32Settings a;
    a.midiChannel = 5;
    a.noteOffset = -12;
    a.sleepTimeoutS = 900;
    a.pressDebounceMs = 11;
    a.hitMode = 1;
    a.hitMinMs = 4;
    a.hitPeakDrop = 50;
    a.hitPeakSamples = 3;
#if defined(PAD_CONFIG_PER_PAD)
    a.padAnalogMode[3] = 1;
    a.padSensitivity[3] = 250;
#else
    a.sensitivity(0) = 200;
    a.lrRatio(0)     = 40;
    a.analogMode(0)  = 1;
    a.velCurve(0)    = 3;
    a.velFloor(0)    = 8;
#endif
    a.save(kv);

    Stm32Settings b;
    b.load(kv);
    REQUIRE(b.midiChannel == 5);
    REQUIRE(b.noteOffset == -12);
    REQUIRE(b.sleepTimeoutS == 900);
    REQUIRE(b.pressDebounceMs == 11);
    REQUIRE(b.hitMode == 1);
    REQUIRE(b.hitMinMs == 4);
    REQUIRE(b.hitPeakDrop == 50);
    REQUIRE(b.hitPeakSamples == 3);
#if defined(PAD_CONFIG_PER_PAD)
    REQUIRE(b.padAnalogMode[3] == 1);
    REQUIRE(b.padSensitivity[3] == 250);
#else
    REQUIRE(b.sensitivity(0)  == 200);
    REQUIRE(b.lrRatio(0)      == 40);
    REQUIRE(b.analogMode(0)   == 1);
    REQUIRE(b.velCurve(0)     == 3);
    REQUIRE(b.velFloor(0)     == 8);
    // all indices alias the one global value
    REQUIRE(b.sensitivity(15) == 200);
    REQUIRE(b.velCurve(15)    == 3);
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
    REQUIRE(s.pressDebounceMs == BOARD_PRESS_DEBOUNCE_MS_DEF);
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

TEST_CASE("Stm32Settings: a store saved before press debounce existed loads 6 ms", "[settings][stm]") {
    // Older firmware never wrote "press_db" (the STM ran its own 15 ms), so such
    // a unit gets the new window on its first boot; one tuned since keeps its value.
    test::MemoryKVStore kv;
    kv.saveU8("cfg_stm", "an_min_ms", 16);   // what a development unit has stored
    kv.saveU8("cfg_stm", "an_delta", 2);
    Stm32Settings old;
    old.load(kv);
    REQUIRE(old.analogMinMs == 16);          // the stream rate cap, not the window
    REQUIRE(old.pressDebounceMs == BOARD_PRESS_DEBOUNCE_MS_DEF);

    old.pressDebounceMs = 10;
    old.save(kv);
    Stm32Settings tuned;
    tuned.load(kv);
    REQUIRE(tuned.pressDebounceMs == 10);
}

TEST_CASE("Stm32Settings: a 4 s hold already in long_press2 loads as 2.4 s", "[settings][stm]") {
    // Core 2.44.2 imported a v1.1.1 backup's 4000 unchanged and saved it under
    // the new key; the next boot repairs it instead of pushing it to the STM.
    test::MemoryKVStore kv;
    kv.saveI32("cfg_stm", "long_press2", 4000);
    Stm32Settings s;
    s.load(kv);
    REQUIRE(s.longPressMs == Stm32Settings::kLongPressDefaultMs);

    kv.saveI32("cfg_stm", "long_press2", 1500);
    s.load(kv);
    REQUIRE(s.longPressMs == 1500);
}

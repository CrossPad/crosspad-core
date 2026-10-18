// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"

using namespace crosspad;

TEST_CASE("CrosspadSettings: default values", "[settings]") {
    auto* settings = CrosspadSettings::getInstance();
    REQUIRE(settings != nullptr);

    // Verify some known defaults exist
    REQUIRE(settings->keypad.enableKeypad == true);
    REQUIRE(settings->LCDbrightness == 100);
}

TEST_CASE("CrosspadSettings: save and load roundtrip", "[settings]") {
    test::MemoryKVStore store;
    store.init();

    auto* settings = CrosspadSettings::getInstance();

    // Modify a setting
    settings->keypad.enableKeypad = false;
    settings->LCDbrightness = 42;

    // Save
    settings->saveTo(store);
    REQUIRE(store.size() > 0);

    // Restore defaults
    settings->keypad.enableKeypad = true;
    settings->LCDbrightness = 100;

    // Load back
    settings->loadFrom(store);
    REQUIRE(settings->keypad.enableKeypad == false);
    REQUIRE(settings->LCDbrightness == 42);

    // Cleanup — restore defaults
    settings->keypad.enableKeypad = true;
}

TEST_CASE("CrosspadSettings: missing keys use defaults", "[settings]") {
    test::MemoryKVStore store;
    store.init();

    auto* settings = CrosspadSettings::getInstance();
    bool originalKeypad = settings->keypad.enableKeypad;

    // Load from empty store — should keep defaults
    settings->loadFrom(store);
    REQUIRE(settings->keypad.enableKeypad == originalKeypad);
}


TEST_CASE("CrosspadSettings: out-of-range NVS values are clamped on load", "[settings]") {
    test::MemoryKVStore store;
    store.init();
    auto* settings = crosspad::CrosspadSettings::getInstance();

    // A corrupt or foreign NVS: brightness above the slider, a volume at 255,
    // a per-codec volume at 200, and a brightness below the slider floor.
    store.saveU8("cfg_display", "brightness", 250);
    store.saveU8("cfg_audio", "out_vol", 255);
    store.saveU8("cfg_audio", "in_vol", 101);
    store.saveU8("cfg_audio", "ovc10", 200);
    settings->loadFrom(store);
    REQUIRE(settings->LCDbrightness == 100);
    REQUIRE(settings->masterFX.outVolume == 100);
    REQUIRE(settings->masterFX.inVolume == 100);
    REQUIRE(settings->masterFX.outVolumeCodec[1][0] == 100);

    store.saveU8("cfg_display", "brightness", 0);
    settings->loadFrom(store);
    REQUIRE(settings->LCDbrightness == 5);

    // In-range values pass through untouched.
    store.saveU8("cfg_display", "brightness", 42);
    store.saveU8("cfg_audio", "out_vol", 7);
    settings->loadFrom(store);
    REQUIRE(settings->LCDbrightness == 42);
    REQUIRE(settings->masterFX.outVolume == 7);

    // Cleanup — back to defaults for the tests that follow.
    settings->LCDbrightness = 100;
    settings->masterFX.outVolume = 50;
    settings->masterFX.inVolume = 50;
}

TEST_CASE("CrosspadSettings: home view round-trips and defaults to the pad map", "[settings]") {
    auto* settings = CrosspadSettings::getInstance();

    test::MemoryKVStore empty;
    empty.init();
    settings->launcherStyle = LauncherStyle::Grid;
    settings->loadFrom(empty);
    REQUIRE(settings->launcherStyle == LauncherStyle::Pads);

    for (LauncherStyle v : { LauncherStyle::Grid, LauncherStyle::List, LauncherStyle::Pads }) {
        test::MemoryKVStore store;
        store.init();
        settings->launcherStyle = v;
        settings->saveTo(store);
        settings->launcherStyle = v == LauncherStyle::Grid ? LauncherStyle::List : LauncherStyle::Grid;
        settings->loadFrom(store);
        REQUIRE(settings->launcherStyle == v);
    }
    settings->launcherStyle = LauncherStyle::Pads;
}

TEST_CASE("CrosspadSettings: the old launcher key does not move a board off the pad map", "[settings]") {
    auto* settings = CrosspadSettings::getInstance();
    test::MemoryKVStore store;
    store.init();
    store.saveU8("cfg_display", "launcher", (uint8_t)LauncherStyle::Grid);
    settings->loadFrom(store);
    REQUIRE(settings->launcherStyle == LauncherStyle::Pads);
}

TEST_CASE("CrosspadSettings: touch sensitivity round-trips, defaults to low, clamps", "[settings]") {
    auto* settings = CrosspadSettings::getInstance();

    test::MemoryKVStore empty;
    empty.init();
    settings->touchSensitivity = TouchSensitivity::High;
    settings->loadFrom(empty);
    REQUIRE(settings->touchSensitivity == TouchSensitivity::Low);

    test::MemoryKVStore store;
    store.init();
    settings->touchSensitivity = TouchSensitivity::Medium;
    settings->saveTo(store);
    settings->touchSensitivity = TouchSensitivity::Low;
    settings->loadFrom(store);
    REQUIRE(settings->touchSensitivity == TouchSensitivity::Medium);

    store.saveU8("cfg_display", "touch_sens", 9);
    settings->loadFrom(store);
    REQUIRE(settings->touchSensitivity == TouchSensitivity::High);
    settings->touchSensitivity = TouchSensitivity::Low;
}

TEST_CASE("CrosspadSettings: enableRemote persists in NVS, missing key means on", "[settings][remote]") {
    auto* settings = CrosspadSettings::getInstance();

    test::MemoryKVStore empty;
    empty.init();
    settings->wireless.enableRemote = false;
    settings->loadFrom(empty);
    REQUIRE(settings->wireless.enableRemote == true);   // a board that never saved it

    test::MemoryKVStore store;
    store.init();
    settings->wireless.enableRemote = false;
    settings->saveTo(store);
    settings->wireless.enableRemote = true;
    settings->loadFrom(store);
    REQUIRE(settings->wireless.enableRemote == false);

    settings->wireless.enableRemote = true;   // leave the singleton as found
}

TEST_CASE("CrosspadSettings: the output queue survives a reboot and a corrupt value does not", "[settings]") {
    test::MemoryKVStore store;
    store.init();
    auto* settings = CrosspadSettings::getInstance();

    settings->audioEngine.queueFrames = 256;
    settings->saveTo(store);
    settings->audioEngine.queueFrames = 0;
    settings->loadFrom(store);
    REQUIRE(settings->audioEngine.queueFrames == 256);

    // Past the bound or negative: the platform's safe default, not a guess.
    store.saveI32("cfg_audio_eng", "q_frames", 100000);
    settings->loadFrom(store);
    REQUIRE(settings->audioEngine.queueFrames == 0);
    store.saveI32("cfg_audio_eng", "q_frames", -5);
    settings->loadFrom(store);
    REQUIRE(settings->audioEngine.queueFrames == 0);
}

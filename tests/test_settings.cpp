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

TEST_CASE("CrosspadSettings: launcher style round-trips and defaults to the list", "[settings]") {
    auto* settings = CrosspadSettings::getInstance();

    test::MemoryKVStore empty;
    empty.init();
    settings->launcherStyle = LauncherStyle::Grid;
    settings->loadFrom(empty);
    REQUIRE(settings->launcherStyle == LauncherStyle::List);

    test::MemoryKVStore store;
    store.init();
    settings->launcherStyle = LauncherStyle::Grid;
    settings->saveTo(store);
    settings->launcherStyle = LauncherStyle::List;
    settings->loadFrom(store);
    REQUIRE(settings->launcherStyle == LauncherStyle::Grid);
    settings->launcherStyle = LauncherStyle::List;
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

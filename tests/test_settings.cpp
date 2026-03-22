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

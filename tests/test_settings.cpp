// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"

#include <map>
#include <string>
#include <vector>

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

namespace {

/* Records which keys a load reads and a save writes, and with which type. */
class KeyRecorder : public crosspad::IKeyValueStore {
public:
    using Keys = std::map<std::string, char>;   // "ns/key" -> 'b' bool, 'u' u8, 'i' i32
    Keys read, saved;
    std::vector<std::string> typeClashes;

    bool init() override { return true; }
    void saveBool(const char* ns, const char* key, bool) override { note(saved, ns, key, 'b'); }
    void saveU8(const char* ns, const char* key, uint8_t) override { note(saved, ns, key, 'u'); }
    void saveI32(const char* ns, const char* key, int32_t) override { note(saved, ns, key, 'i'); }
    bool readBool(const char* ns, const char* key, bool d) override { note(read, ns, key, 'b'); return d; }
    uint8_t readU8(const char* ns, const char* key, uint8_t d) override { note(read, ns, key, 'u'); return d; }
    int32_t readI32(const char* ns, const char* key, int32_t d) override { note(read, ns, key, 'i'); return d; }
    void eraseAll() override {}

private:
    void note(Keys& into, const char* ns, const char* key, char type) {
        const std::string name = std::string(ns) + "/" + key;
        const auto it = into.find(name);
        if (it != into.end() && it->second != type) typeClashes.push_back(name);
        into[name] = type;
    }
};

/* NVS refuses a namespace or a key longer than this (NVS_KEY_NAME_MAX_SIZE - 1). */
constexpr size_t kNvsNameMax = 15;

}  // namespace

TEST_CASE("CrosspadSettings: load and save use the same keys with the same types", "[settings]") {
    auto* settings = CrosspadSettings::getInstance();
    KeyRecorder store;
    settings->saveTo(store);
    settings->loadFrom(store);

    REQUIRE_FALSE(store.saved.empty());
    CHECK(store.typeClashes.empty());
    for (const auto& [name, type] : store.saved) {
        INFO("saved and never loaded, or loaded as another type: " << name);
        const auto it = store.read.find(name);
        REQUIRE(it != store.read.end());
        CHECK(it->second == type);
    }
    for (const auto& [name, type] : store.read) {
        INFO("loaded and never saved: " << name);
        CHECK(store.saved.count(name) == 1);
    }
}

TEST_CASE("CrosspadSettings: every namespace and key fits NVS", "[settings]") {
    auto* settings = CrosspadSettings::getInstance();
    KeyRecorder store;
    settings->saveTo(store);
    settings->loadFrom(store);

    for (const auto* keys : {&store.saved, &store.read}) {
        for (const auto& [name, type] : *keys) {
            INFO(name);
            const size_t slash = name.find('/');
            CHECK(slash <= kNvsNameMax);
            CHECK(name.size() - slash - 1 <= kNvsNameMax);
        }
    }
}

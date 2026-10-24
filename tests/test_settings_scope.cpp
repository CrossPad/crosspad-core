// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "crosspad/settings/SettingsScope.hpp"
#include "crosspad/settings/SettingsIo.hpp"

#include <cstdio>

using namespace crosspad;

TEST_CASE("per-firmware import keeps shared settings", "[settings][scope]") {
    CrosspadSettings& s = *CrosspadSettings::getInstance();
    const char* path = "scope_test_settings.json";

    s.LCDbrightness = 20; s.usbSerialGuard = false; s.themeColorIndex = 3; s.Kit = 5;
    REQUIRE(settingsExportAll(s, path).ok);

    s.LCDbrightness = 90; s.usbSerialGuard = true; s.themeColorIndex = 1; s.Kit = 9;
    REQUIRE(settingsImportPerFirmware(s, path).ok);

    CHECK(s.themeColorIndex == 3);      // per firmware: comes from the file
    CHECK(s.Kit == 5);
    CHECK(s.LCDbrightness == 90);       // shared: the device's value survives
    CHECK(s.usbSerialGuard == true);
    std::remove(path);
}

TEST_CASE("a failed import changes nothing", "[settings][scope]") {
    CrosspadSettings& s = *CrosspadSettings::getInstance();
    s.themeColorIndex = 4;
    CHECK_FALSE(settingsImportPerFirmware(s, "does_not_exist.json").ok);
    CHECK(s.themeColorIndex == 4);
}

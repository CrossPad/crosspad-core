// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>

#include "crosspad/app/AppVersions.hpp"
#include "crosspad/settings/InfoRowTable.hpp"

using crosspad::InfoRowTable;

// The table a platform's build generates; these tests link this one instead.
namespace crosspad {
namespace {
const AppVersion kVersions[] = {
    {"crosspad-core", "", "83d8c7e", "main", false},
    {"crosspad-sampler", "sampler", "a5d9912", "v0.8.4", false},
    {"crosspad-mixer", "mixer", "1950a9e", "", true},
    {"crosspad-song", nullptr, "04e5758", "", false},
    {"crosspad-arrange", "arrange", "920798f", nullptr, false},
};
} // namespace
std::size_t appVersionCount() { return sizeof(kVersions) / sizeof(kVersions[0]); }
const AppVersion* appVersionGet(std::size_t idx) {
    return idx < appVersionCount() ? &kVersions[idx] : nullptr;
}
} // namespace crosspad

static std::string valueAt(const InfoRowTable& t, int idx, std::string* label = nullptr) {
    const char* l = nullptr;
    const char* v = nullptr;
    REQUIRE(t.get(idx, &l, &v));
    if (label) *label = l;
    return v;
}

TEST_CASE("InfoRowTable builds once until invalidated", "[settings]") {
    InfoRowTable t;
    REQUIRE(t.beginBuild());
    t.add("Firmware", "%s", "v1.2.0");
    t.add("PCB revision", "%u", 20u);
    CHECK_FALSE(t.beginBuild());
    CHECK(t.count() == 2);

    t.invalidate();
    REQUIRE(t.beginBuild());
    CHECK(t.count() == 0);
}

TEST_CASE("InfoRowTable formats, cuts long values and stops when full", "[settings]") {
    InfoRowTable t;
    t.beginBuild();
    t.add("MAC", "%02X%02X", 0xAB, 0x01);
    std::string label;
    CHECK(valueAt(t, 0, &label) == "AB01");
    CHECK(label == "MAC");

    const std::string longValue(InfoRowTable::kValueMax + 10, 'x');
    t.add("Long", "%s", longValue.c_str());
    CHECK(valueAt(t, 1).size() == InfoRowTable::kValueMax - 1);

    for (int i = t.count(); i < InfoRowTable::kMaxRows + 3; ++i) t.add("Row", "%d", i);
    CHECK(t.count() == InfoRowTable::kMaxRows);
    CHECK(valueAt(t, InfoRowTable::kMaxRows - 1) == std::to_string(InfoRowTable::kMaxRows - 1));

    const char* l = nullptr;
    const char* v = nullptr;
    CHECK_FALSE(t.get(-1, &l, &v));
    CHECK_FALSE(t.get(InfoRowTable::kMaxRows, &l, &v));
}

TEST_CASE("InfoRowTable lists installed apps, not core or gui", "[settings]") {
    InfoRowTable t;
    t.beginBuild();
    t.addAppVersions();
    REQUIRE(t.count() == 3);
    std::string label;
    CHECK(valueAt(t, 0, &label) == "a5d9912 @v0.8.4");
    CHECK(label == "crosspad-sampler");
    CHECK(valueAt(t, 1, &label) == "1950a9e *");
    CHECK(label == "crosspad-mixer");
    CHECK(valueAt(t, 2, &label) == "920798f");
    CHECK(label == "crosspad-arrange");
}

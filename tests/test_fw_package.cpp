// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include "crosspad/fw/FwPackage.hpp"

using namespace crosspad;

TEST_CASE("package: every verdict reads as words", "[fw]") {
    for (uint8_t v = 0; v <= static_cast<uint8_t>(FwPkgVerdict::Downloading); ++v) {
        const char* text = fwPkgVerdictText(static_cast<FwPkgVerdict>(v));
        CHECK(text[0] != '\0');
        CHECK(std::strcmp(text, "unknown") != 0);
    }
    CHECK(std::strcmp(fwPkgVerdictText(FwPkgVerdict::WrongBoard), "built for another board revision") == 0);
}

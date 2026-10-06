#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include "crosspad/fw/FwKit.hpp"

using namespace crosspad;

TEST_CASE("kit ids", "[fw]") {
    CHECK(fwKitIdValid("full"));
    CHECK(fwKitIdValid("my_kit2"));
    CHECK(fwKitIdValid("abcdefghijklmno"));          // 15 characters
    CHECK_FALSE(fwKitIdValid("abcdefghijklmnop"));   // 16: does not fit CPFI kit_id[16]
    CHECK_FALSE(fwKitIdValid(""));
    CHECK_FALSE(fwKitIdValid(nullptr));
    CHECK_FALSE(fwKitIdValid("2kit"));
    CHECK_FALSE(fwKitIdValid("Full"));
    CHECK_FALSE(fwKitIdValid("my-kit"));
    CHECK_FALSE(fwKitIdValid("v1"));                 // would read as a release tag
    CHECK_FALSE(fwKitIdValid("v10"));
    CHECK(fwKitIdValid("v1a"));
    CHECK(fwKitIdValid("vx"));
    CHECK(fwKitIdValid("v"));
}

TEST_CASE("beta kit list", "[fw]") {
    char list[kFwBetaListMax] = "";
    CHECK_FALSE(fwBetaListHas(list, "full"));
    REQUIRE(fwBetaListSet(list, sizeof list, "full", true));
    REQUIRE(fwBetaListSet(list, sizeof list, "lite", true));
    CHECK(fwBetaListHas(list, "full"));
    CHECK(fwBetaListHas(list, "lite"));
    CHECK_FALSE(fwBetaListHas(list, "ful"));          // whole names only
    REQUIRE(fwBetaListSet(list, sizeof list, "full", true));
    CHECK(std::strcmp(list, "full lite") == 0);       // idempotent
    REQUIRE(fwBetaListSet(list, sizeof list, "full", false));
    CHECK(std::strcmp(list, "lite") == 0);
    REQUIRE(fwBetaListSet(list, sizeof list, "nope", false));   // clearing an absent kit is fine
    CHECK_FALSE(fwBetaListSet(list, sizeof list, "Bad Kit", true));
    char tiny[6] = "";
    CHECK(fwBetaListSet(tiny, sizeof tiny, "full", true));
    CHECK_FALSE(fwBetaListSet(tiny, sizeof tiny, "lite", true));  // no room: list untouched
    CHECK(std::strcmp(tiny, "full") == 0);
}

#include <catch2/catch_test_macros.hpp>

#include "crosspad/kit/KitPathUtils.hpp"

using namespace crosspad;

TEST_CASE("kit paths: a logical path gets the mount in front", "[kit]") {
    setKitPathFsPrefix("/sdcard");
    CHECK(kitFsPath("/crosspad/kits/BASIC/SAMPLES/1.wav") == "/sdcard/crosspad/kits/BASIC/SAMPLES/1.wav");
    CHECK(kitFsPath("").empty());
    setKitPathFsPrefix("");
    CHECK(kitFsPath("/crosspad/kits/BASIC/1.wav") == "/crosspad/kits/BASIC/1.wav");
}

TEST_CASE("kit paths: a sample with fx lives beside it, a file per setting", "[kit]") {
    const std::string s = "/crosspad/kits/BASIC/SAMPLES/1.wav";
    CHECK(renderedSamplePath(s, SampleFx{3, 0, 0}) == "/crosspad/kits/BASIC/SAMPLES/.fx/1_p+3.wav");
    CHECK(renderedSamplePath("/k/kick.WAV", SampleFx{-12, 200, 8000}) == "/k/.fx/kick_p-12_h200_l8000.wav");
    CHECK(renderedSamplePath(s, SampleFx{0, 0, 5000}) == "/crosspad/kits/BASIC/SAMPLES/.fx/1_l5000.wav");
    CHECK(renderedSamplePath(s, SampleFx{0, 0, 0, true}) == "/crosspad/kits/BASIC/SAMPLES/.fx/1_r.wav");
    CHECK(renderedSamplePath(s, SampleFx{}) == s);
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "crosspad/audio/AudioFormat.hpp"

using namespace crosspad;

TEST_CASE("AudioFormat: bytes per sample", "[audio][format]") {
    REQUIRE(audioFormatBytesPerSample(AudioFormat::Float32) == 4);
    REQUIRE(audioFormatBytesPerSample(AudioFormat::Int32)   == 4);
    REQUIRE(audioFormatBytesPerSample(AudioFormat::Int16)   == 2);
}

TEST_CASE("AudioFormat: mask bit values", "[audio][format]") {
    REQUIRE(audioFormatMask(AudioFormat::Float32) == 0x01);
    REQUIRE(audioFormatMask(AudioFormat::Int32)   == 0x02);
    REQUIRE(audioFormatMask(AudioFormat::Int16)   == 0x04);
}

TEST_CASE("AudioFormat: supports() bitmask check", "[audio][format]") {
    const uint32_t all = audioFormatMask(AudioFormat::Float32) |
                         audioFormatMask(AudioFormat::Int32)   |
                         audioFormatMask(AudioFormat::Int16);

    REQUIRE(audioFormatSupports(all, AudioFormat::Float32));
    REQUIRE(audioFormatSupports(all, AudioFormat::Int32));
    REQUIRE(audioFormatSupports(all, AudioFormat::Int16));

    const uint32_t int16Only = audioFormatMask(AudioFormat::Int16);
    REQUIRE(audioFormatSupports(int16Only, AudioFormat::Int16));
    REQUIRE_FALSE(audioFormatSupports(int16Only, AudioFormat::Float32));
    REQUIRE_FALSE(audioFormatSupports(int16Only, AudioFormat::Int32));
}

TEST_CASE("AudioFormat: preferred picks highest quality", "[audio][format]") {
    SECTION("All three → Float32") {
        const uint32_t mask = audioFormatMask(AudioFormat::Float32) |
                              audioFormatMask(AudioFormat::Int32)   |
                              audioFormatMask(AudioFormat::Int16);
        REQUIRE(audioFormatPreferred(mask) == AudioFormat::Float32);
    }
    SECTION("Int32+Int16 → Int32") {
        const uint32_t mask = audioFormatMask(AudioFormat::Int32) |
                              audioFormatMask(AudioFormat::Int16);
        REQUIRE(audioFormatPreferred(mask) == AudioFormat::Int32);
    }
    SECTION("Int16 only → Int16") {
        REQUIRE(audioFormatPreferred(audioFormatMask(AudioFormat::Int16)) == AudioFormat::Int16);
    }
    SECTION("Float32 only → Float32") {
        REQUIRE(audioFormatPreferred(audioFormatMask(AudioFormat::Float32)) == AudioFormat::Float32);
    }
    SECTION("Empty mask → Int16 fallback") {
        REQUIRE(audioFormatPreferred(0) == AudioFormat::Int16);
    }
}

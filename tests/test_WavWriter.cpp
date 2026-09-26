// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include "crosspad/audio/WavWriter.hpp"
#include "crosspad/instrument/WavPcm.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

using namespace crosspad;

TEST_CASE("WavWriter: a stereo take reads back through WavPcm", "[audio][wav]") {
    const char* p = "cp_wavwriter_rt.wav";
    std::vector<int16_t> s(2 * 1000);
    for (size_t i = 0; i < 1000; ++i) {
        s[2 * i]     = static_cast<int16_t>(i);
        s[2 * i + 1] = static_cast<int16_t>(-static_cast<int>(i));
    }

    WavWriter w;
    REQUIRE(w.open(p, 44100, 2));
    REQUIRE(w.write(s.data(), 600));
    REQUIRE(w.write(s.data() + 2 * 600, 400));
    REQUIRE(w.frames() == 1000);
    REQUIRE(w.close());

    WavInfo info;
    REQUIRE(wavReadInfo(p, info));
    REQUIRE(info.sampleRate == 44100);
    REQUIRE(info.channels == 2);
    REQUIRE(info.bits == 16);
    REQUIRE(info.frames == 1000);
    REQUIRE(std::filesystem::file_size(p) == 44 + 1000 * 4);
    std::filesystem::remove(p);
}

TEST_CASE("WavWriter: the data chunk counts written frames, not reserved ones", "[audio][wav]") {
    const char* p = "cp_wavwriter_res.wav";
    std::vector<int16_t> s(2 * 100, 7);

    WavWriter w;
    REQUIRE(w.open(p, 48000, 2));
    REQUIRE(w.reserve(10000));
    REQUIRE(w.write(s.data(), 100));
    REQUIRE(w.close());

    WavInfo info;
    REQUIRE(wavReadInfo(p, info));
    REQUIRE(info.frames == 100);
    std::filesystem::remove(p);
}

TEST_CASE("WavWriter: a reservation past what a WAV can address is refused", "[audio][wav]") {
    const char* p = "cp_wavwriter_big.wav";
    WavWriter w;
    REQUIRE(w.open(p, 48000, 2));
    // 2^30 stereo frames = 4 GiB of data: over the RIFF 32-bit size field.
    REQUIRE_FALSE(w.reserve(1u << 30));
    REQUIRE(w.close());
    REQUIRE(std::filesystem::file_size(p) == 44);
    std::filesystem::remove(p);
}

TEST_CASE("WavWriter: open refuses a zero rate or channel count", "[audio][wav]") {
    WavWriter w;
    REQUIRE_FALSE(w.open("cp_wavwriter_bad.wav", 0, 2));
    REQUIRE_FALSE(w.open("cp_wavwriter_bad.wav", 44100, 0));
    REQUIRE_FALSE(w.isOpen());
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "crosspad/audio/WavWriter.hpp"
#include "crosspad/instrument/WavPcm.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include <unistd.h>

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

TEST_CASE("WavWriter: frames written past stdio are counted by advance and sized by close", "[audio][wav]") {
    const char* p = "cp_wavwriter_raw.wav";
    std::vector<int16_t> s(2 * 2000);
    for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<int16_t>(i);

    for (const bool reserved : {true, false}) {
        CAPTURE(reserved);
        WavWriter w;
        REQUIRE(w.open(p, 44100, 2));
        if (reserved) REQUIRE(w.reserve(2000));
        /* Unreserved, the header is still in stdio's buffer: the flush puts
         * it out before the frames go to the descriptor. */
        auto rawWrite = [&](const int16_t* frames, uint32_t n) {
            REQUIRE(std::fflush(w.stream()) == 0);
            REQUIRE(::write(fileno(w.stream()), frames, n * 4) == static_cast<ssize_t>(n * 4));
            w.advance(n);
        };
        rawWrite(s.data(), 1013);
        rawWrite(s.data() + 2 * 1013, 987);
        REQUIRE(w.frames() == 2000);
        REQUIRE(w.close());

        WavInfo info;
        REQUIRE(wavReadInfo(p, info));
        CHECK(info.channels == 2);
        CHECK(info.frames == 2000);
        REQUIRE(std::filesystem::file_size(p) == WavWriter::kHeaderBytes + 2000 * 4);
        std::vector<int16_t> back(s.size());
        std::FILE* f = std::fopen(p, "rb");
        REQUIRE(f);
        REQUIRE(std::fseek(f, WavWriter::kHeaderBytes, SEEK_SET) == 0);
        REQUIRE(std::fread(back.data(), 2, back.size(), f) == back.size());
        std::fclose(f);
        CHECK(back == s);
        std::filesystem::remove(p);
    }
}

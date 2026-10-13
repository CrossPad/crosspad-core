// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/**
 * @file wav_fixture.hpp
 * @brief RIFF/WAVE writers shared by the tests that need a real file on disk.
 *
 * WavPcm and PitchedKitLoader both read through stdio, so their tests write
 * actual files into the platform temp directory. Everything here builds the
 * container by hand rather than through a library: the point of several cases
 * is a header the writer would refuse to produce.
 */

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace test {

inline void putU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF);
    v.push_back((x >> 16) & 0xFF); v.push_back((x >> 24) & 0xFF);
}

inline void putU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF);
}

inline void putTag(std::vector<uint8_t>& v, const char* t) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(t[i]));
}

/// Wrap @p body in a RIFF/WAVE container and write it to @p path.
inline void writeRiff(const std::string& path, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> file;
    putTag(file, "RIFF");
    putU32(file, static_cast<uint32_t>(4 + body.size()));
    putTag(file, "WAVE");
    file.insert(file.end(), body.begin(), body.end());

    FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    std::fwrite(file.data(), 1, file.size(), f);
    std::fclose(f);
}

/// A "fmt " chunk for uncompressed PCM.
inline void putFmtChunk(std::vector<uint8_t>& body, uint16_t channels, uint16_t bits,
                        uint32_t rate) {
    const uint16_t frameBytes = static_cast<uint16_t>(channels * (bits / 8));
    putTag(body, "fmt ");
    putU32(body, 16);
    putU16(body, 1);              // WAVE_FORMAT_PCM
    putU16(body, channels);
    putU32(body, rate);
    putU32(body, rate * frameBytes);
    putU16(body, frameBytes);
    putU16(body, bits);
}

/// A "data" chunk of 16-bit samples, in whatever channel order the caller
/// interleaved them.
inline void putDataChunk(std::vector<uint8_t>& body, const std::vector<int16_t>& samples) {
    putTag(body, "data");
    putU32(body, static_cast<uint32_t>(samples.size() * sizeof(int16_t)));
    for (int16_t s : samples) putU16(body, static_cast<uint16_t>(s));
}

/// Absolute path of @p name inside the platform temp directory.
inline std::string tempPath(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

/**
 * @brief Write a WAV to the temp directory and return its path.
 *
 * A deliberately odd-sized LIST chunk sits between "fmt " and "data", so every
 * reader that walks this file has to honour RIFF's padding byte.
 */
inline std::string writeWav(const std::string& name, uint16_t channels, uint16_t bits,
                            uint32_t rate, const std::vector<int16_t>& samples) {
    std::vector<uint8_t> body;
    putFmtChunk(body, channels, bits, rate);

    const char* list = "INFOx";        // 5 bytes -> odd chunk, needs a pad byte
    putTag(body, "LIST");
    putU32(body, 5);
    for (int i = 0; i < 5; ++i) body.push_back(static_cast<uint8_t>(list[i]));
    body.push_back(0);

    putDataChunk(body, samples);

    const std::string path = tempPath(name);
    writeRiff(path, body);
    return path;
}

/// Mono 16-bit sine of @p hz, faded in and out so the ends are not a step.
inline std::vector<int16_t> sineSamples(double hz, uint32_t rate, double seconds,
                                        double peak = 20000.0) {
    const uint32_t frames = static_cast<uint32_t>(rate * seconds);
    std::vector<int16_t> out(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        out[i] = static_cast<int16_t>(peak * std::sin(2.0 * M_PI * hz * i / rate));
    }
    return out;
}

/// Mono 16-bit sine written straight to @p path (no temp-directory lookup, no
/// LIST chunk) — the shape a kit's zone file has.
inline void writeSineWav(const std::string& path, double hz, uint32_t rate, double seconds) {
    std::vector<uint8_t> body;
    putFmtChunk(body, 1, 16, rate);
    putDataChunk(body, sineSamples(hz, rate, seconds));
    writeRiff(path, body);
}

} // namespace test

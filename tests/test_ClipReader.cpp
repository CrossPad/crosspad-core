#include <catch2/catch_test_macros.hpp>
#include "crosspad/audio/WavWriter.hpp"
#include "../src/song/SongEngineImpl.hpp"

#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace crosspad;

namespace {

/// A WAV of @p frames whose every sample says where it is.
std::string writeCounting(const char* name, uint16_t channels, uint32_t frames) {
    const auto dir = std::filesystem::temp_directory_path() / "cp_clip_reader";
    std::filesystem::create_directories(dir);
    const std::string path = (dir / name).string();
    WavWriter w;
    REQUIRE(w.open(path.c_str(), 44100, channels));
    std::vector<int16_t> pcm((size_t)frames * channels);
    for (uint32_t f = 0; f < frames; ++f)
        for (uint16_t c = 0; c < channels; ++c) pcm[(size_t)f * channels + c] = (int16_t)(f * 3 + c);
    REQUIRE(w.write(pcm.data(), frames));
    REQUIRE(w.close());
    return path;
}

} // namespace

TEST_CASE("ClipReader: the read-ahead reads what a plain read does", "[song][reader]") {
    for (uint16_t channels : {uint16_t(2), uint16_t(1)}) {
        const uint32_t frames = 20011;   // the end inside a sector, and inside a run
        const std::string path = writeCounting(channels == 2 ? "stereo.wav" : "mono.wav", channels, frames);
        ClipReader plain, ahead;
        REQUIRE(plain.open(path));
        REQUIRE(ahead.open(path));
        std::vector<uint8_t> buf(kAheadBytes);
        ahead.setAhead(buf.data(), kAheadBytes);
        std::mt19937 rng(channels);
        uint32_t frame = 0;
        for (int i = 0; i < 400; ++i) {
            /* Mostly on from the last chunk, as playback reads; a jump now and then. */
            if (rng() % 8 == 0) frame = rng() % (frames + 100);
            const uint32_t n = 1 + rng() % 1500;
            std::vector<int16_t> a((size_t)n * 2, 7), b((size_t)n * 2, 9);
            const bool wa = plain.readWhole(frame, a.data(), n);
            const bool wb = ahead.readWhole(frame, b.data(), n);
            REQUIRE(wa == wb);
            REQUIRE(a == b);
            frame += n;
            if (frame > frames + 50) frame = 0;
        }
    }
}

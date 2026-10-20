// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "crosspad/instrument/WavPcm.hpp"
#include "wav_fixture.hpp"

#include <filesystem>
#include <string>
#include <vector>

using namespace crosspad;
using test::writeWav;

TEST_CASE("wavReadInfo: mono 16-bit with a LIST chunk", "[instrument][wav]") {
    std::vector<int16_t> s(1000);
    for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<int16_t>(i);
    const std::string p = writeWav("cp_wav_mono.wav", 1, 16, 44100, s);

    WavInfo info;
    REQUIRE(wavReadInfo(p.c_str(), info));
    REQUIRE(info.sampleRate == 44100);
    REQUIRE(info.channels == 1);
    REQUIRE(info.bits == 16);
    REQUIRE(info.frames == 1000);

    std::vector<int16_t> out(1000, -1);
    REQUIRE(wavReadMono16(p.c_str(), out.data(), 1000));
    for (size_t i = 0; i < 1000; ++i) REQUIRE(out[i] == static_cast<int16_t>(i));

    std::filesystem::remove(p);
}

TEST_CASE("wavReadMono16: stereo is averaged", "[instrument][wav]") {
    // L = i, R = i + 100 -> mono should be i + 50
    std::vector<int16_t> s(2 * 500);
    for (size_t i = 0; i < 500; ++i) {
        s[2 * i]     = static_cast<int16_t>(i);
        s[2 * i + 1] = static_cast<int16_t>(i + 100);
    }
    const std::string p = writeWav("cp_wav_stereo.wav", 2, 16, 48000, s);

    WavInfo info;
    REQUIRE(wavReadInfo(p.c_str(), info));
    REQUIRE(info.channels == 2);
    REQUIRE(info.sampleRate == 48000);
    REQUIRE(info.frames == 500);

    std::vector<int16_t> out(500, -1);
    REQUIRE(wavReadMono16(p.c_str(), out.data(), 500));
    for (size_t i = 0; i < 500; ++i) REQUIRE(out[i] == static_cast<int16_t>(i + 50));

    std::filesystem::remove(p);
}

TEST_CASE("wavReadMono16: short file leaves the tail zeroed", "[instrument][wav]") {
    std::vector<int16_t> s(64, 1234);
    const std::string p = writeWav("cp_wav_short.wav", 1, 16, 44100, s);

    std::vector<int16_t> out(200, -1);
    REQUIRE(wavReadMono16(p.c_str(), out.data(), 200));
    for (size_t i = 0; i < 64; ++i) REQUIRE(out[i] == 1234);
    for (size_t i = 64; i < 200; ++i) REQUIRE(out[i] == 0);

    std::filesystem::remove(p);
}

TEST_CASE("wavReadMono16: reads more than one 4 kB chunk", "[instrument][wav]") {
    std::vector<int16_t> s(9000);
    for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<int16_t>((i * 7) % 30000 - 15000);
    const std::string p = writeWav("cp_wav_big.wav", 1, 16, 44100, s);

    std::vector<int16_t> out(9000, 0);
    REQUIRE(wavReadMono16(p.c_str(), out.data(), 9000));
    for (size_t i = 0; i < s.size(); ++i) REQUIRE(out[i] == s[i]);

    std::filesystem::remove(p);
}

TEST_CASE("wavReadInfo: rejects unsupported formats", "[instrument][wav]") {
    WavInfo info;
    REQUIRE_FALSE(wavReadInfo("/definitely/not/a/file.wav", info));
    REQUIRE_FALSE(wavReadInfo(nullptr, info));

    std::vector<int16_t> s(100, 0);
    const std::string p = writeWav("cp_wav_8bit.wav", 1, 8, 44100, s);
    REQUIRE_FALSE(wavReadInfo(p.c_str(), info));
    std::filesystem::remove(p);
}

namespace {

/// A WAV whose data chunk declares @p declaredSize bytes regardless of what
/// actually follows it.
std::string writeWavLyingData(const std::string& name, uint32_t declaredSize,
                              const std::vector<int16_t>& samples) {
    std::vector<uint8_t> body;
    test::putFmtChunk(body, 1, 16, 44100);
    test::putTag(body, "data");
    test::putU32(body, declaredSize);
    for (int16_t s : samples) test::putU16(body, static_cast<uint16_t>(s));

    const std::string path = test::tempPath(name);
    test::writeRiff(path, body);
    return path;
}

} // namespace

TEST_CASE("wavReadInfo: a data chunk claiming 4 GB reports the real frames",
          "[instrument][wav]") {
    // Chunk sizes are attacker- (or corruption-) controlled: 0xFFFFFFF0 does
    // not fit a 32-bit long, so a reader that seeks past it unchecked goes
    // negative and reports frames the file does not hold.
    std::vector<int16_t> s(300);
    for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<int16_t>(i * 3);
    const std::string p = writeWavLyingData("cp_wav_huge.wav", 0xFFFFFFF0u, s);

    WavInfo info;
    REQUIRE(wavReadInfo(p.c_str(), info));
    REQUIRE(info.channels == 1);
    REQUIRE(info.frames == 300);

    std::vector<int16_t> out(400, -1);
    REQUIRE(wavReadMono16(p.c_str(), out.data(), 400));
    for (size_t i = 0; i < 300; ++i) REQUIRE(out[i] == static_cast<int16_t>(i * 3));
    for (size_t i = 300; i < 400; ++i) REQUIRE(out[i] == 0);

    std::filesystem::remove(p);
}

TEST_CASE("wavReadInfo: a truncated file reports the bytes that are there",
          "[instrument][wav]") {
    std::vector<int16_t> s(1000);
    for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<int16_t>(i);
    const std::string p = writeWav("cp_wav_trunc.wav", 1, 16, 44100, s);

    // Cut 600 of the 1000 frames away; the header still claims all of them.
    const auto full = std::filesystem::file_size(p);
    std::filesystem::resize_file(p, full - 600 * 2);

    WavInfo info;
    REQUIRE(wavReadInfo(p.c_str(), info));
    REQUIRE(info.frames == 400);

    std::vector<int16_t> out(1000, -1);
    REQUIRE(wavReadMono16(p.c_str(), out.data(), 1000));
    for (size_t i = 0; i < 400; ++i) REQUIRE(out[i] == static_cast<int16_t>(i));
    for (size_t i = 400; i < 1000; ++i) REQUIRE(out[i] == 0);

    std::filesystem::remove(p);
}

TEST_CASE("wavReadInfo: a header cut short is refused", "[instrument][wav]") {
    std::vector<int16_t> s(100, 7);
    const std::string p = writeWav("cp_wav_stub.wav", 1, 16, 44100, s);
    std::filesystem::resize_file(p, 8);          // less than the RIFF header

    WavInfo info;
    REQUIRE_FALSE(wavReadInfo(p.c_str(), info));
    std::filesystem::remove(p);
}

TEST_CASE("WavPcmReader: the chunk size the caller picks changes nothing",
          "[instrument][wav]") {
    // The reader is what a streaming load pulls a zone through, so the piece
    // size has to be the caller's business: 1 frame, either side of the
    // internal 4 kB buffer, or the whole file at once.
    std::vector<int16_t> s(9000);
    for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<int16_t>((i * 7) % 30000 - 15000);
    const std::string p = writeWav("cp_wav_chunks.wav", 1, 16, 44100, s);

    for (uint32_t chunk : { 1u, 4095u, 4096u, 9000u }) {
        WavPcmReader r;
        REQUIRE(r.open(p.c_str()));
        REQUIRE(r.info().frames == 9000);

        std::vector<int16_t> out(9000, 0);
        uint32_t done = 0;
        while (done < 9000) {
            const uint32_t got = r.readMono16(out.data() + done, chunk);
            if (got == 0) break;
            done += got;
        }
        INFO("chunk = " << chunk);
        REQUIRE(done == 9000);
        REQUIRE(out == s);
        REQUIRE(r.readMono16(out.data(), chunk) == 0u);   // and the file is spent
    }

    std::filesystem::remove(p);
}

TEST_CASE("WavPcmReader: stereo is averaged across chunk boundaries",
          "[instrument][wav]") {
    std::vector<int16_t> s(2 * 3000);
    for (size_t i = 0; i < 3000; ++i) {
        s[2 * i]     = static_cast<int16_t>(i);
        s[2 * i + 1] = static_cast<int16_t>(i + 100);
    }
    const std::string p = writeWav("cp_wav_chunks_st.wav", 2, 16, 48000, s);

    WavPcmReader r;
    REQUIRE(r.open(p.c_str()));
    REQUIRE(r.info().channels == 2);
    REQUIRE(r.info().frames == 3000);

    // 1023 frames is 4092 bytes: every read stops just short of the internal
    // buffer, so a stereo pair straddles one only if the reader lets it.
    std::vector<int16_t> out(3000, -1);
    uint32_t done = 0;
    while (done < 3000) {
        const uint32_t got = r.readMono16(out.data() + done, 1023);
        if (got == 0) break;
        done += got;
    }
    REQUIRE(done == 3000);
    for (size_t i = 0; i < 3000; ++i) REQUIRE(out[i] == static_cast<int16_t>(i + 50));

    std::filesystem::remove(p);
}

TEST_CASE("WavPcmReader: a rejected file still reports what was parsed",
          "[instrument][wav]") {
    std::vector<int16_t> s(100, 0);
    const std::string p = writeWav("cp_wav_reader_8bit.wav", 1, 8, 44100, s);

    WavPcmReader r;
    REQUIRE_FALSE(r.open(p.c_str()));
    REQUIRE(r.info().bits == 8);
    REQUIRE(r.info().sampleRate == 44100);
    std::vector<int16_t> out(10, 0);
    REQUIRE(r.readMono16(out.data(), 10) == 0u);

    std::filesystem::remove(p);
}

namespace {

struct OpenFile {
    FILE* f;
    explicit OpenFile(const std::string& p) : f(std::fopen(p.c_str(), "rb")) { REQUIRE(f != nullptr); }
    ~OpenFile() { std::fclose(f); }
};

/// A WAVE_FORMAT_EXTENSIBLE fmt chunk (40 bytes) for @p bits-bit samples in
/// containers of @p containerBytes bytes.
void putExtensibleFmt(std::vector<uint8_t>& body, uint16_t channels, uint16_t bits,
                      uint16_t containerBytes, uint32_t rate) {
    const uint16_t blockAlign = static_cast<uint16_t>(channels * containerBytes);
    test::putTag(body, "fmt ");
    test::putU32(body, 40);
    test::putU16(body, 0xFFFE);
    test::putU16(body, channels);
    test::putU32(body, rate);
    test::putU32(body, rate * blockAlign);
    test::putU16(body, blockAlign);
    test::putU16(body, static_cast<uint16_t>(containerBytes * 8));
    test::putU16(body, 22);                       // cbSize
    test::putU16(body, bits);                     // valid bits
    test::putU32(body, 3);                        // channel mask
    for (int i = 0; i < 16; ++i) body.push_back(static_cast<uint8_t>(i));   // subformat GUID
}

} // namespace

TEST_CASE("wavReadLayout: extensible fmt, odd LIST chunk, 24-bit frames", "[instrument][wav]") {
    std::vector<uint8_t> body;
    putExtensibleFmt(body, 2, 24, 3, 48000);
    test::putTag(body, "LIST");
    test::putU32(body, 3);                        // odd: one pad byte follows
    body.insert(body.end(), {'a', 'b', 'c', 0});
    test::putTag(body, "data");
    test::putU32(body, 6 * 10);
    body.insert(body.end(), 6 * 10, 0x11);
    const std::string p = test::tempPath("cp_wav_layout_ext.wav");
    test::writeRiff(p, body);

    OpenFile f(p);
    WavLayout l;
    REQUIRE(wavReadLayout(f.f, l, false));
    CHECK(l.format == 0xFFFE);
    CHECK(l.channels == 2);
    CHECK(l.rate == 48000);
    CHECK(l.blockAlign == 6);
    CHECK(l.bits == 24);
    CHECK(l.dataOffset == 12 + (8 + 40) + (8 + 4) + 8);
    CHECK(l.dataBytes == 60);

    std::filesystem::remove(p);
}

TEST_CASE("wavReadLayout: a data chunk ahead of the fmt chunk", "[instrument][wav]") {
    std::vector<uint8_t> body;
    test::putDataChunk(body, std::vector<int16_t>(7, 3));   // 14 bytes
    test::putFmtChunk(body, 1, 16, 22050);
    const std::string p = test::tempPath("cp_wav_layout_order.wav");
    test::writeRiff(p, body);

    OpenFile f(p);
    WavLayout l;
    REQUIRE(wavReadLayout(f.f, l, true));
    CHECK(l.dataOffset == 12 + 8);
    CHECK(l.dataBytes == 14);
    CHECK(l.rate == 22050);
    CHECK(l.blockAlign == 2);

    WavLayout engine;                            // the engine frames a data chunk only after its fmt
    CHECK_FALSE(engine.parse(f.f));

    std::filesystem::remove(p);
}

TEST_CASE("wavReadLayout: declared sizes are trusted unless clamped", "[instrument][wav]") {
    std::vector<int16_t> s(100, 5);
    const std::string p = writeWavLyingData("cp_wav_layout_lie.wav", 1000, s);

    OpenFile f(p);
    WavLayout l;
    REQUIRE(wavReadLayout(f.f, l, false));
    CHECK(l.dataBytes == 1000);
    REQUIRE(wavReadLayout(f.f, l, true));
    CHECK(l.dataBytes == 200);

    std::filesystem::remove(p);
}

TEST_CASE("wavReadLayout: refuses what is not a WAV, keeps what it parsed", "[instrument][wav]") {
    WavLayout l;
    CHECK_FALSE(wavReadLayout(nullptr, l, true));

    std::vector<uint8_t> body;
    test::putFmtChunk(body, 2, 16, 44100);       // no data chunk
    const std::string p = test::tempPath("cp_wav_layout_nodata.wav");
    test::writeRiff(p, body);
    {
        OpenFile f(p);
        CHECK_FALSE(wavReadLayout(f.f, l, false));
        CHECK(l.channels == 2);
        CHECK(l.rate == 44100);
    }

    FILE* w = std::fopen(p.c_str(), "wb");
    REQUIRE(w != nullptr);
    std::fputs("RIFX....WAVEfmt ", w);
    std::fclose(w);
    {
        OpenFile f(p);
        CHECK_FALSE(wavReadLayout(f.f, l, false));
        CHECK_FALSE(wavReadLayout(f.f, l, true));
    }
    std::filesystem::remove(p);
}

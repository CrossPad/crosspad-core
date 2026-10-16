// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "crosspad/dsp/FftResampler.hpp"
#include "crosspad/dsp/PortableFft.hpp"
#include "crosspad/kit/KitInfo.hpp"
#include "crosspad/kit/KitPathUtils.hpp"
#include "crosspad/kit/KitRateConvert.hpp"
#include "../src/kit/KitJsonParser.hpp"
#include "../src/util/JsonStdio.hpp"
#include "wav_fixture.hpp"

#include <ArduinoJson.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace crosspad;
namespace fs = std::filesystem;

namespace {

struct TestHost : IKitConvertHost {
    PortableFft f;
    uint64_t free = UINT64_MAX;
    size_t live = 0, peak = 0;
    IFft& fft() override { return f; }
    uint64_t freeBytes(const std::string&) override { return free; }
    void* alloc(size_t n) override {
        void* p = std::malloc(n + sizeof(size_t));
        if (!p) return nullptr;
        *static_cast<size_t*>(p) = n;
        live += n;
        peak = std::max(peak, live);
        return static_cast<size_t*>(p) + 1;
    }
    void release(void* p) override {
        if (!p) return;
        size_t* h = static_cast<size_t*>(p) - 1;
        live -= *h;
        std::free(h);
    }
};

std::string g_root;

/// A fresh tree under the temp directory, used as the card: logical and
/// filesystem paths are the same (no mount prefix).
void freshRoot() {
    g_root = (fs::temp_directory_path() / "cp_kit441").string();
    fs::remove_all(g_root);
    fs::create_directories(g_root);
    setKitPathFsPrefix("");
}

std::string readText(const std::string& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeText(const std::string& p, const std::string& s) {
    fs::create_directories(fs::path(p).parent_path());
    std::ofstream(p) << s;
}

/// A 16-bit WAV, @p channels interleaved, a sine per channel; optional chunks
/// before and after the data.
void writeWav(const std::string& path, uint16_t channels, uint32_t rate, uint32_t frames,
              const std::vector<uint8_t>& before = {}, const std::vector<uint8_t>& after = {},
              uint16_t bits = 16) {
    fs::create_directories(fs::path(path).parent_path());
    std::vector<uint8_t> body;
    test::putFmtChunk(body, channels, bits, rate);
    body.insert(body.end(), before.begin(), before.end());
    std::vector<int16_t> s(static_cast<size_t>(frames) * channels);
    for (uint32_t i = 0; i < frames; ++i)
        for (uint16_t c = 0; c < channels; ++c)
            s[i * channels + c] = static_cast<int16_t>(12000 * std::sin(2 * M_PI * (440.0 * (c + 1)) * i / rate));
    if (bits == 16) {
        test::putDataChunk(body, s);
    } else {
        test::putTag(body, "data");
        test::putU32(body, frames * channels * 3);
        for (size_t i = 0; i < s.size(); ++i) { body.push_back(0); test::putU16(body, static_cast<uint16_t>(s[i])); }
    }
    body.insert(body.end(), after.begin(), after.end());
    test::writeRiff(path, body);
}

std::vector<uint8_t> smplChunk(uint32_t rate, uint32_t loopStart, uint32_t loopEnd) {
    std::vector<uint8_t> c;
    test::putTag(c, "smpl");
    test::putU32(c, 36 + 24);
    test::putU32(c, 0); test::putU32(c, 0);
    test::putU32(c, 1000000000u / rate);
    test::putU32(c, 60); test::putU32(c, 0); test::putU32(c, 0); test::putU32(c, 0);
    test::putU32(c, 1);  // one loop
    test::putU32(c, 0);
    test::putU32(c, 0); test::putU32(c, 0);   // cue id, type
    test::putU32(c, loopStart); test::putU32(c, loopEnd);
    test::putU32(c, 0); test::putU32(c, 0);
    return c;
}

WavLayout layoutOf(const std::string& p) {
    WavLayout l;
    FILE* f = std::fopen(p.c_str(), "rb");
    REQUIRE(f);
    REQUIRE(l.parse(f));
    std::fclose(f);
    return l;
}

std::vector<uint8_t> chunkBytes(const std::string& p, const char* id) {
    const WavLayout l = layoutOf(p);
    for (const auto& c : l.chunks) {
        if (std::memcmp(c.id, id, 4) != 0) continue;
        std::vector<uint8_t> out(c.size);
        FILE* f = std::fopen(p.c_str(), "rb");
        std::fseek(f, c.offset, SEEK_SET);
        REQUIRE(std::fread(out.data(), 1, c.size, f) == c.size);
        std::fclose(f);
        return out;
    }
    return {};
}

uint32_t u32(const std::vector<uint8_t>& v, size_t at) {
    return v[at] | v[at + 1] << 8 | v[at + 2] << 16 | static_cast<uint32_t>(v[at + 3]) << 24;
}

KitInfo parseKit(const std::string& path) {
    JsonDocument doc;
    std::ifstream in(path);
    REQUIRE(!deserializeJson(doc, in));
    KitInfo k;
    kit_parse_metadata(k, doc);
    kit_parse_pads(k, doc);
    return k;
}

KitRateConverter::Phase runAll(KitRateConverter& c, int maxSteps = 1000000) {
    while (maxSteps-- > 0 && c.step()) {}
    return c.phase();
}

/// kit A: five pads of every kind the conversion meets, one borrowed sample,
/// one already at the rate, one it cannot read; and fields of its own.
std::string makeKitA() {
    const std::string kit = g_root + "/kits/kitA";
    writeWav(kit + "/kick.wav", 1, 48000, 48000, smplChunk(48000, 4800, 47999));
    writeWav(kit + "/SAMPLES/snare.wav", 2, 48000, 24000);
    writeWav(kit + "/loop.wav", 2, 48000, 96000, {}, smplChunk(48000, 0, 95999));
    writeWav(g_root + "/library/hat.wav", 1, 48000, 12000);
    writeWav(kit + "/ok.wav", 1, 44100, 4410);
    writeWav(kit + "/deep.wav", 1, 48000, 4800, {}, {}, 24);
    writeText(kit + "/kit.json",
              "{\"name\":\"Kit A\",\"custom\":\"kept\",\"pads\":["
              "{\"samples\":[{\"sample\":\"kick.wav\",\"start\":0,\"end\":0}],\"label\":\"x\"},"
              "{\"samples\":[\"snare.wav\"]},"
              "{\"samples\":[{\"sample\":\"loop.wav\",\"start\":48000,\"end\":96000},"
                            "{\"sample\":\"loop.wav\",\"start\":0,\"end\":24000}]},"
              "{\"samples\":[{\"sample\":\"" + g_root + "/library/hat.wav\"}]},"
              "{\"samples\":[{\"sample\":\"ok.wav\"}]},"
              "{\"samples\":[{\"sample\":\"deep.wav\"}]}]}");
    return kit + "/kit.json";
}

double sineSnrDb(const std::vector<int16_t>& got, double hz, uint32_t rate, double peak, uint32_t skip) {
    double sig = 0, err = 0;
    for (size_t i = skip; i + skip < got.size(); ++i) {
        const double want = peak * std::sin(2 * M_PI * hz * i / rate);
        sig += want * want;
        err += (got[i] - want) * (got[i] - want);
    }
    return 10 * std::log10(sig / std::max(err, 1e-9));
}

std::vector<int16_t> resampleWith(uint32_t fftMax, uint32_t oversample, const std::vector<int16_t>& in,
                                  uint32_t from, uint32_t to) {
    PortableFft fft(fftMax);
    FftResamplerStream st;
    REQUIRE(st.begin(fft, from, to, std::malloc, std::free, oversample));
    std::vector<int16_t> out(resampledLength(static_cast<uint32_t>(in.size()), from, to));
    uint32_t n = st.push(in.data(), static_cast<uint32_t>(in.size()), out.data(), static_cast<uint32_t>(out.size()));
    st.flush(out.data() + n, static_cast<uint32_t>(out.size()) - n);
    return out;
}

} // namespace

TEST_CASE("rate folder names read as a rate", "[KitRateConvert]") {
    CHECK(rateFolderName(44100) == "44k1");
    CHECK(rateFolderName(48000) == "48k");
    CHECK(rateFolderName(22050) == "22k05");
    CHECK(rateFolderName(96000) == "96k");
    CHECK(scaleFrame(48000, 48000, 44100) == 44100);
    CHECK(scaleFrame(1, 48000, 44100) == 1);
    CHECK(scaleFrame(0, 48000, 44100) == 0);
    CHECK(scaleFrame(22050, 22050, 44100) == 44100);
}

TEST_CASE("metadata positions follow the new rate", "[KitRateConvert]") {
    auto c = smplChunk(48000, 48000, 95999);
    std::vector<uint8_t> p(c.begin() + 8, c.end());
    REQUIRE(scaleWavChunk("smpl", p.data(), static_cast<uint32_t>(p.size()), 48000, 44100));
    CHECK(u32(p, 8) == 22676);                  // ns per sample at 44.1 kHz
    CHECK(u32(p, 36 + 8) == 44100);             // loop start
    CHECK(u32(p, 36 + 12) == 88199);            // loop end, inclusive: same length

    std::vector<uint8_t> cue;
    test::putU32(cue, 1);
    test::putU32(cue, 7); test::putU32(cue, 4800); test::putTag(cue, "data");
    test::putU32(cue, 0); test::putU32(cue, 0); test::putU32(cue, 4800);
    REQUIRE(scaleWavChunk("cue ", cue.data(), static_cast<uint32_t>(cue.size()), 48000, 44100));
    CHECK(u32(cue, 4 + 4) == 4410);
    CHECK(u32(cue, 4 + 20) == 4410);

    std::vector<uint8_t> list;
    test::putTag(list, "adtl");
    test::putTag(list, "ltxt"); test::putU32(list, 20);
    test::putU32(list, 7); test::putU32(list, 9600);
    for (int i = 0; i < 12; ++i) list.push_back(0);
    REQUIRE(scaleWavChunk("LIST", list.data(), static_cast<uint32_t>(list.size()), 48000, 44100));
    CHECK(u32(list, 4 + 8 + 4) == 8820);

    std::vector<uint8_t> bext(602, 0);
    bext[338] = 0x80; bext[339] = 0xBB;         // 48000
    REQUIRE(scaleWavChunk("bext", bext.data(), static_cast<uint32_t>(bext.size()), 48000, 44100));
    CHECK(u32(bext, 338) == 44100);

    std::vector<uint8_t> peak(16, 0);
    CHECK_FALSE(scaleWavChunk("PEAK", peak.data(), 16, 48000, 44100));
    CHECK_FALSE(scaleWavChunk("JUNK", peak.data(), 16, 48000, 44100));
    CHECK(scaleWavChunk("acid", peak.data(), 16, 48000, 44100));
}

TEST_CASE("the converter's resampler is clean to the top of the band", "[KitRateConvert]") {
    // The pitched loader's settings (2x, block 4096) against the converter's
    // (8x, block 256): the read-out's images are the difference, not the block.
    const uint32_t from = 48000, to = 44100;
    const struct { double hz; double floorDb; } tones[] = {
        {1000.0, 85.0}, {10000.0, 80.0}, {19000.0, 65.0}};
    for (const auto& t : tones) {
        auto in = test::sineSamples(t.hz, from, 1.0, 20000.0);
        const double pitched = sineSnrDb(resampleWith(kMaxFftSize, 2, in, from, to), t.hz, to, 20000.0, 4096);
        const double conv = sineSnrDb(resampleWith(kConvertFftMax, kConvertOversample, in, from, to),
                                      t.hz, to, 20000.0, 4096);
        INFO(t.hz << " Hz: 2x " << pitched << " dB, converter " << conv << " dB");
        CHECK(conv > t.floorDb);
        CHECK(conv >= pitched - 0.5);
    }
    // Above the new Nyquist: what is left is alias, folded to 21.1 kHz. The
    // 256-sample block's transition band is what lets it through; 1024 would
    // take it to -85 dB for twice the scratch.
    auto in = test::sineSamples(23000.0, from, 1.0, 20000.0);
    auto out = resampleWith(kConvertFftMax, kConvertOversample, in, from, to);
    double e = 0;
    for (size_t i = 4096; i + 4096 < out.size(); ++i) e += double(out[i]) * out[i];
    const double rmsDb = 10 * std::log10(e / (out.size() - 8192) / (20000.0 * 20000.0 / 2));
    INFO("23 kHz alias residue " << rmsDb << " dB");
    CHECK(rmsDb < -55.0);
}

TEST_CASE("a kit is converted, retargeted and tidied", "[KitRateConvert]") {
    freshRoot();
    const std::string json = makeKitA();
    const std::string kit = g_root + "/kits/kitA";
    // Another kit borrows loop.wav from this one by a relative path.
    writeText(g_root + "/kits/kitB/kit.json",
              "{\"name\":\"B\",\"pads\":[{\"samples\":[{\"sample\":\"../kitA/loop.wav\"}]},"
              "{\"samples\":[{\"sample\":\"kick.wav\"}]}]}");
    writeWav(g_root + "/kits/kitB/kick.wav", 1, 44100, 100);

    TestHost host;
    KitRateConverter c;
    REQUIRE(c.begin(host, json, 44100));
    REQUIRE(runAll(c) == KitRateConverter::Phase::Done);
    const auto& p = c.progress();
    CHECK(p.files == 6);
    CHECK(p.toConvert == 4);
    CHECK(p.converted == 4);
    CHECK(p.skipped == 1);           // the 24-bit one
    CHECK(p.fromRate == 48000);
    CHECK(host.live == 0);
    INFO("peak working memory " << host.peak);
    CHECK(host.peak < 80 * 1024);

    // The files: at the rate, the right length, the loop scaled.
    auto kick = layoutOf(kit + "/44k1/kick.wav");
    CHECK(kick.rate == 44100);
    CHECK(kick.channels == 1);
    CHECK(kick.frames == 44100);
    auto smpl = chunkBytes(kit + "/44k1/kick.wav", "smpl");
    REQUIRE(smpl.size() == 60);
    CHECK(u32(smpl, 36 + 8) == 4410);
    CHECK(u32(smpl, 36 + 12) == 44099);
    auto loop = layoutOf(kit + "/44k1/loop.wav");
    CHECK(loop.channels == 2);
    CHECK(loop.frames == 88200);
    REQUIRE(!loop.chunks.empty());
    CHECK(loop.chunks.back().afterData);
    CHECK(u32(chunkBytes(kit + "/44k1/loop.wav", "smpl"), 36 + 12) == 88199);
    CHECK(layoutOf(kit + "/44k1/SAMPLES/snare.wav").frames == 22050);
    CHECK(layoutOf(kit + "/44k1/_shared/hat.wav").frames == 11025);
    CHECK_FALSE(fs::exists(kit + "/44k1/ok.wav"));
    CHECK_FALSE(fs::exists(kit + "/44k1/kick.wav.part"));

    // The JSON: retargeted, trims scaled, everything else as it was.
    const std::string text = readText(json);
    CHECK(text.find("\"custom\":\"kept\"") != std::string::npos);
    CHECK(text.find("\"label\":\"x\"") != std::string::npos);
    KitInfo k = parseKit(json);
    CHECK(k.pads[0].samples[0].sample == "44k1/kick.wav");
    CHECK(k.pads[1].samples[0].sample == "44k1/SAMPLES/snare.wav");
    CHECK(k.pads[2].samples[0].sample == "44k1/loop.wav");
    CHECK(k.pads[2].samples[0].start == 44100);
    CHECK(k.pads[2].samples[0].end == 88200);
    CHECK(k.pads[2].samples[1].end == 22050);
    CHECK(k.pads[3].samples[0].sample == "44k1/_shared/hat.wav");
    CHECK(k.pads[4].samples[0].sample == "ok.wav");
    CHECK(k.pads[5].samples[0].sample == "deep.wav");
    CHECK(resolveKitSamplePath(json, k.pads[1].samples[0].sample) == kit + "/44k1/SAMPLES/snare.wav");

    // What the model in memory is told.
    const auto& edits = c.edits();
    REQUIRE(edits.size() == 5);
    CHECK(edits[2].pad == 2);
    CHECK(edits[2].oldSample == "loop.wav");
    CHECK(edits[2].oldStart == 48000);
    CHECK(edits[2].start == 44100);

    // Nothing removed yet: the originals wait for the tidy.
    CHECK(fs::exists(kit + "/kick.wav"));
    CHECK(kitTidyPending(json, 44100));

    KitTidyResult t = tidyKit(host, json, g_root + "/kits", 44100);
    CHECK(t.ok);
    CHECK(t.removed == 2);           // kick, snare
    CHECK(t.kept == 1);              // loop.wav: kit B names it
    CHECK(t.pending == 0);
    CHECK_FALSE(fs::exists(kit + "/kick.wav"));
    CHECK_FALSE(fs::exists(kit + "/SAMPLES"));      // emptied, gone
    CHECK(fs::exists(kit + "/loop.wav"));
    CHECK(fs::exists(g_root + "/library/hat.wav"));  // borrowed: never touched
    CHECK(fs::exists(g_root + "/kits/kitB/kick.wav"));
    CHECK(fs::exists(kit + "/deep.wav"));
    CHECK_FALSE(kitTidyPending(json, 44100));

    // Again: nothing to do, nothing offered.
    KitRateConverter again;
    REQUIRE(again.begin(host, json, 44100));
    CHECK(runAll(again) == KitRateConverter::Phase::Done);
    CHECK(again.progress().toConvert == 0);
    CHECK(again.edits().empty());
}

TEST_CASE("a stopped conversion leaves the kit as it was and resumes", "[KitRateConvert]") {
    freshRoot();
    const std::string json = makeKitA();
    const std::string kit = g_root + "/kits/kitA";
    const std::string before = readText(json);

    TestHost host;
    KitRateConverter c;
    REQUIRE(c.begin(host, json, 44100));
    // Into the second file to be written (snare), then stopped.
    while (c.step() && !(c.phase() == KitRateConverter::Phase::Converting &&
                         c.progress().fileIndex == 2 && c.progress().framesDone > 48000 + 1024)) {}
    REQUIRE(c.progress().fileIndex == 2);
    c.cancel();
    CHECK_FALSE(c.step());
    CHECK(c.phase() == KitRateConverter::Phase::Cancelled);
    CHECK(host.live == 0);
    CHECK(readText(json) == before);                       // the kit never moved
    CHECK(fs::exists(kit + "/44k1/kick.wav"));             // finished: kept
    CHECK_FALSE(fs::exists(kit + "/44k1/SAMPLES/snare.wav.part"));
    CHECK_FALSE(fs::exists(kit + "/44k1/SAMPLES/snare.wav"));

    // A torn write from a power cut: a .part, and a target that does not verify.
    writeText(kit + "/44k1/loop.wav.part", "half");
    writeText(kit + "/44k1/loop.wav", "RIFF....");

    KitRateConverter resume;
    REQUIRE(resume.begin(host, json, 44100));
    REQUIRE(runAll(resume) == KitRateConverter::Phase::Done);
    CHECK(resume.progress().converted == 4);
    CHECK(layoutOf(kit + "/44k1/loop.wav").frames == 88200);
    CHECK_FALSE(fs::exists(kit + "/44k1/loop.wav.part"));
    CHECK(parseKit(json).pads[0].samples[0].sample == "44k1/kick.wav");
}

TEST_CASE("a list written before the kit was retargeted removes nothing", "[KitRateConvert]") {
    freshRoot();
    const std::string json = makeKitA();
    const std::string kit = g_root + "/kits/kitA";
    // The commit's first half only: the list is there, the JSON is the old one.
    writeText(kit + "/44k1/.originals", "kick.wav\nloop.wav\n");
    TestHost host;
    KitTidyResult t = tidyKit(host, json, g_root + "/kits", 44100);
    CHECK(t.ok);
    CHECK(t.removed == 0);
    CHECK(t.kept == 2);
    CHECK(fs::exists(kit + "/kick.wav"));
    CHECK(fs::exists(kit + "/loop.wav"));
}

TEST_CASE("a full card is refused before anything is written", "[KitRateConvert]") {
    freshRoot();
    const std::string json = makeKitA();
    TestHost host;
    host.free = 100 * 1024;
    KitRateConverter c;
    REQUIRE(c.begin(host, json, 44100));
    CHECK(runAll(c) == KitRateConverter::Phase::Failed);
    CHECK(std::string(c.progress().error).find("card full") == 0);
    CHECK_FALSE(fs::exists(g_root + "/kits/kitA/44k1"));
}

TEST_CASE("pitched kits and missing kits are not converted", "[KitRateConvert]") {
    freshRoot();
    writeText(g_root + "/kits/p/kit.json",
              "{\"engine\":\"pitched\",\"instrument\":{\"zones\":[{\"sample\":\"a.wav\"}]},\"pads\":[]}");
    TestHost host;
    KitRateConverter c;
    REQUIRE(c.begin(host, g_root + "/kits/p/kit.json", 44100));
    CHECK(runAll(c) == KitRateConverter::Phase::Failed);
    CHECK(std::string(c.progress().error) == "pitched kit");

    KitRateConverter m;
    REQUIRE(m.begin(host, g_root + "/kits/none/kit.json", 44100));
    CHECK(runAll(m) == KitRateConverter::Phase::Failed);
}

TEST_CASE("keep_rate is written, read and kept by a save", "[KitRateConvert]") {
    freshRoot();
    const std::string json = makeKitA();
    TestHost host;
    REQUIRE(setKitKeepRate(host, json, true));
    KitInfo k = parseKit(json);
    CHECK(k.keepRate);
    CHECK(readText(json).find("\"custom\":\"kept\"") != std::string::npos);

    JsonDocument doc;
    kit_serialize(k, doc);
    CHECK(doc["keep_rate"].as<bool>());

    REQUIRE(setKitKeepRate(host, json, false));
    CHECK_FALSE(parseKit(json).keepRate);
    CHECK(readText(json).find("keep_rate") == std::string::npos);
}

TEST_CASE("an upsampled file has the length of the new rate", "[KitRateConvert]") {
    freshRoot();
    const std::string kit = g_root + "/kits/lofi";
    writeWav(kit + "/a.wav", 2, 22050, 22050);
    writeText(kit + "/kit.json", "{\"pads\":[{\"samples\":[{\"sample\":\"a.wav\",\"start\":11025,\"end\":0}]}]}");
    TestHost host;
    KitRateConverter c;
    REQUIRE(c.begin(host, kit + "/kit.json", 44100));
    REQUIRE(runAll(c) == KitRateConverter::Phase::Done);
    CHECK(layoutOf(kit + "/44k1/a.wav").frames == 44100);
    KitInfo k = parseKit(kit + "/kit.json");
    CHECK(k.pads[0].samples[0].start == 22050);
    CHECK(k.pads[0].samples[0].end == 0);
}

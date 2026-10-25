// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "crosspad/dsp/FftResampler.hpp"
#include "crosspad/dsp/PortableFft.hpp"
#include "crosspad/kit/KitInfo.hpp"
#include "crosspad/kit/KitPathUtils.hpp"
#include "crosspad/kit/KitRateConvert.hpp"
#include "crosspad/kit/KitSaved.hpp"
#include "../src/kit/KitJsonParser.hpp"
#include "../src/util/JsonStdio.hpp"
#include "wav_fixture.hpp"

#include <ArduinoJson.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace crosspad;
namespace fs = std::filesystem;

namespace {

struct TestHost : IKitConvertHost {
    PortableFft f;
    uint64_t free = UINT64_MAX;
    std::string busy;      // a file something still reads
    bool releaseFile(const char* p) override { return busy != p; }
    size_t live = 0, peak = 0;
    IFft& fft() override { return f; }
    uint64_t freeBytes(const char*) override { return free; }
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

/// This process's folder under the temp directory: ctest -j runs the cases
/// side by side, and a shared one was emptied under the case still using it.
/// Removed when the process ends.
const std::string& processRoot() {
    struct Root {
        std::string path = (fs::temp_directory_path() /
                            ("cp_kit441_" + std::to_string(std::random_device{}()) + "_" +
                             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
                               .string();
        ~Root() { std::error_code ec; fs::remove_all(path, ec); }
    };
    static const Root root;
    return root.path;
}

/// A fresh tree under the temp directory, used as the card: logical and
/// filesystem paths are the same (no mount prefix).
void freshRoot() {
    g_root = processRoot();
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
    WavLayout l;
    WavLayout::Chunk list[16];
    FILE* lf = std::fopen(p.c_str(), "rb");
    REQUIRE(lf);
    REQUIRE(l.parse(lf, list, 16));
    std::fclose(lf);
    for (uint8_t i = 0; i < l.chunkCount; ++i) {
        const auto& c = list[i];
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
    char b[kRateFolderMax];
    CHECK(std::string(rateFolderName(44100, b)) == "44k1");
    CHECK(std::string(rateFolderName(48000, b)) == "48k");
    CHECK(std::string(rateFolderName(22050, b)) == "22k05");
    CHECK(std::string(rateFolderName(96000, b)) == "96k");
    CHECK(std::string(rateFolderName(11025, b)) == "11k025");
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
    const uint32_t saves = kit_saved_generation(json);
    REQUIRE(c.begin(host, json, 44100));
    REQUIRE(runAll(c) == KitRateConverter::Phase::Done);
    CHECK(kit_saved_generation(json) == saves + 1);   // the kit.json it rewrote
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
    {
        WavLayout l;
        WavLayout::Chunk list[16];
        FILE* lf = std::fopen((kit + "/44k1/loop.wav").c_str(), "rb");
        REQUIRE(lf);
        REQUIRE(l.parse(lf, list, 16));
        std::fclose(lf);
        REQUIRE(l.chunkCount > 0);
        CHECK(list[l.chunkCount - 1].afterData);
    }
    CHECK(u32(chunkBytes(kit + "/44k1/loop.wav", "smpl"), 36 + 12) == 88199);
    CHECK(layoutOf(kit + "/44k1/SAMPLES/snare.wav").frames == 22050);
    CHECK(layoutOf(kit + "/44k1/_shared/hat.wav").frames == 11025);
    CHECK_FALSE(fs::exists(kit + "/44k1/ok.wav"));
    CHECK_FALSE(fs::exists(kit + "/44k1/kick.wav.part"));

    // The JSON: retargeted, trims scaled, saved as every kit save is.
    KitInfo k = parseKit(json);
    CHECK(k.name == "Kit A");
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

TEST_CASE("an original something still reads waits for the next tidy", "[KitRateConvert]") {
    freshRoot();
    const std::string json = makeKitA();
    const std::string kit = g_root + "/kits/kitA";
    TestHost host;
    KitRateConverter c;
    REQUIRE(c.begin(host, json, 44100));
    REQUIRE(runAll(c) == KitRateConverter::Phase::Done);
    host.busy = kit + "/kick.wav";
    KitTidyResult t = tidyKit(host, json, g_root + "/kits", 44100);
    CHECK(t.removed == 2);           // snare, loop (no other kit here)
    CHECK(t.pending == 1);
    CHECK(fs::exists(kit + "/kick.wav"));
    CHECK(kitTidyPending(json, 44100));
    host.busy.clear();
    t = tidyKit(host, json, g_root + "/kits", 44100);
    CHECK(t.removed == 1);
    CHECK_FALSE(fs::exists(kit + "/kick.wav"));
    CHECK_FALSE(kitTidyPending(json, 44100));
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

TEST_CASE("keep_rate is read and kept by a save", "[KitRateConvert]") {
    JsonDocument in;
    REQUIRE(!deserializeJson(in, "{\"name\":\"k\",\"keep_rate\":true,\"pads\":[]}"));
    KitInfo k;
    kit_parse_metadata(k, in);
    CHECK(k.keepRate);
    JsonDocument out;
    kit_serialize(k, out);
    CHECK(out["keep_rate"].as<bool>());
    k.keepRate = false;
    JsonDocument none;
    kit_serialize(k, none);
    CHECK(none["keep_rate"].isNull());
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

// ── Untrusted card contents (security review, 16.10.2026) ──────────────────

TEST_CASE("an adtl list whose ltxt size wraps the arithmetic stays in its buffer", "[KitRateConvert][untrusted]") {
    // "adtl" + "ltxt" + a size that wraps at + 8 + sz to 4, and four bytes:
    // the check passed, the length was written past the end, and the cursor
    // never moved again.
    std::vector<uint8_t> list;
    test::putTag(list, "adtl");
    test::putTag(list, "ltxt");
    test::putU32(list, 0xFFFFFFF8u);
    test::putU32(list, 0x11223344u);
    REQUIRE(list.size() == 16);
    std::vector<uint8_t> copy = list;
    CHECK(scaleWavChunk("LIST", list.data(), static_cast<uint32_t>(list.size()), 48000, 44100));
    CHECK(list == copy);
}

TEST_CASE("frame positions past the 32-bit range are clamped, not wrapped", "[KitRateConvert][untrusted]") {
    CHECK(scaleFrame(0xFFFFFFF0u, 22050, 44100) == UINT32_MAX);
    auto c = smplChunk(22050, 0xFFFFFFF0u, 0xFFFFFFFFu);
    std::vector<uint8_t> p(c.begin() + 8, c.end());
    REQUIRE(scaleWavChunk("smpl", p.data(), static_cast<uint32_t>(p.size()), 22050, 44100));
    CHECK(u32(p, 36 + 8) == UINT32_MAX);
    CHECK(u32(p, 36 + 12) == UINT32_MAX);
}

TEST_CASE("a sample named with .. is never written or removed outside the kit", "[KitRateConvert][untrusted]") {
    freshRoot();
    const std::string kit = g_root + "/kits/kitA";
    writeWav(g_root + "/victim.wav", 1, 48000, 4800);
    writeWav(kit + "/own.wav", 1, 48000, 4800);
    writeText(kit + "/kit.json",
              "{\"pads\":[{\"samples\":[{\"sample\":\"../../victim.wav\"}]},"
              "{\"samples\":[{\"sample\":\"SAMPLES/../own.wav\"}]}]}");
    TestHost host;
    KitRateConverter c;
    REQUIRE(c.begin(host, kit + "/kit.json", 44100));
    REQUIRE(runAll(c) == KitRateConverter::Phase::Done);
    CHECK_FALSE(fs::exists(g_root + "/kits/victim.wav"));      // written outside before
    CHECK(fs::exists(kit + "/44k1/_shared/victim.wav"));       // a borrowed file: copied in
    // A dotted spelling is not the kit's own (review 2): never removed. Here
    // it does not even open -- SAMPLES/ is not there -- as on the board.
    KitTidyResult t = tidyKit(host, kit + "/kit.json", g_root + "/kits", 44100);
    CHECK(t.ok);
    CHECK(fs::exists(g_root + "/victim.wav"));                 // removed before
    CHECK(fs::exists(kit + "/own.wav"));
    for (const auto& e : fs::recursive_directory_iterator(g_root)) {
        const std::string p = e.path().string();
        INFO(p);
        CHECK((p.rfind(kit, 0) == 0 || p == g_root + "/victim.wav" || p == g_root + "/kits"));
    }
}

TEST_CASE("the tidy's list cannot point it outside the kit's samples", "[KitRateConvert][untrusted]") {
    freshRoot();
    const std::string json = makeKitA();
    const std::string kit = g_root + "/kits/kitA";
    writeWav(g_root + "/victim.wav", 1, 48000, 100);
    writeWav(kit + "/44k1/conv.wav", 1, 44100, 100);
    writeWav(kit + "/stray.wav", 1, 48000, 100);
    writeText(kit + "/44k1/.originals",
              "../../victim.wav\n" + g_root + "/victim.wav\nkit.json\n44k1/conv.wav\n"
              "SAMPLES/../../kitA/kit.json\nstray.wav\n");
    TestHost host;
    KitTidyResult t = tidyKit(host, json, g_root + "/kits", 44100);
    CHECK(t.ok);
    CHECK(fs::exists(g_root + "/victim.wav"));
    CHECK(fs::exists(json));
    CHECK(fs::exists(kit + "/44k1/conv.wav"));
    CHECK_FALSE(fs::exists(kit + "/stray.wav"));               // a sample of the kit's own, named by nothing
    CHECK(t.removed == 1);
}

TEST_CASE("a file that would not fit a WAV once converted is not converted", "[KitRateConvert][untrusted]") {
    freshRoot();
    const std::string kit = g_root + "/kits/huge";
    // 8 kHz stereo, 900 MB of data, sparse: at 44.1 kHz it would be ~5 GB --
    // past what the RIFF and data sizes can say. Was planned, and every size
    // computed in 32 bits wrapped.
    std::vector<uint8_t> body;
    test::putFmtChunk(body, 2, 16, 8000);
    test::putTag(body, "data");
    const uint32_t dataBytes = 900u * 1024 * 1024;
    test::putU32(body, dataBytes);
    fs::create_directories(kit);
    test::writeRiff(kit + "/big.wav", body);
    fs::resize_file(kit + "/big.wav", 12 + body.size() + dataBytes);
    // writeRiff wrote the RIFF size for the header alone; say what follows.
    {
        FILE* f = std::fopen((kit + "/big.wav").c_str(), "r+b");
        REQUIRE(f);
        std::fseek(f, 4, SEEK_SET);
        const uint32_t riff = static_cast<uint32_t>(4 + body.size() + dataBytes);
        std::fwrite(&riff, 4, 1, f);
        std::fclose(f);
    }
    writeText(kit + "/kit.json", "{\"pads\":[{\"samples\":[{\"sample\":\"big.wav\"}]}]}");
    TestHost host;
    KitRateConverter c;
    REQUIRE(c.begin(host, kit + "/kit.json", 44100));
    while (c.phase() == KitRateConverter::Phase::Planning && c.step()) {}
    CHECK(c.progress().toConvert == 0);
    CHECK(c.progress().skipped == 1);
    fs::remove(kit + "/big.wav");
}

TEST_CASE("a chunk that claims more than the file holds ends the walk", "[KitRateConvert][untrusted]") {
    freshRoot();
    const std::string p = g_root + "/odd.wav";
    std::vector<uint8_t> body;
    test::putFmtChunk(body, 1, 16, 48000);
    test::putTag(body, "data");
    test::putU32(body, 0x7FFFFFF0u);             // declared far past the file
    for (int i = 0; i < 100; ++i) test::putU16(body, static_cast<uint16_t>(i));
    test::putTag(body, "smpl");                  // inside the data, as the file says
    test::writeRiff(p, body);
    WavLayout l;
    WavLayout::Chunk list[4];
    FILE* f = std::fopen(p.c_str(), "rb");
    REQUIRE(f);
    REQUIRE(l.parse(f, list, 4));
    std::fclose(f);
    CHECK(l.dataBytes == 100 * 2 + 4);
    CHECK(l.frames == 102);
    CHECK(l.chunkCount == 0);
}

TEST_CASE("big metadata chunks of a mono file are copied whole", "[KitRateConvert][untrusted]") {
    // A mono file's sample buffers are smaller than a 4 kB piece: an unknown
    // chunk was copied through one of them a 4 kB piece at a time.
    freshRoot();
    const std::string kit = g_root + "/kits/meta";
    std::vector<uint8_t> ixml, info;
    test::putTag(ixml, "iXML");
    test::putU32(ixml, 10001);
    for (int i = 0; i < 10001; ++i) ixml.push_back(static_cast<uint8_t>(i * 7));
    ixml.push_back(0);                                  // pad byte
    test::putTag(info, "LIST");
    test::putU32(info, 9000);
    test::putTag(info, "INFO");
    for (int i = 0; i < 8996; ++i) info.push_back(static_cast<uint8_t>(i));
    writeWav(kit + "/m.wav", 1, 48000, 48000, ixml, info);
    writeText(kit + "/kit.json", "{\"pads\":[{\"samples\":[{\"sample\":\"m.wav\"}]}]}");
    TestHost host;
    KitRateConverter c;
    REQUIRE(c.begin(host, kit + "/kit.json", 44100));
    REQUIRE(runAll(c) == KitRateConverter::Phase::Done);
    auto x = chunkBytes(kit + "/44k1/m.wav", "iXML");
    REQUIRE(x.size() == 10001);
    CHECK(x[10000] == static_cast<uint8_t>(10000 * 7));
    auto l = chunkBytes(kit + "/44k1/m.wav", "LIST");
    REQUIRE(l.size() == 9000);
    CHECK(l[8999] == static_cast<uint8_t>(8995));
    CHECK(layoutOf(kit + "/44k1/m.wav").frames == 44100);
}

// ── Second review (16.10.2026): one parser, exact references, no stale buffers ──

namespace {
std::vector<uint8_t> fmtBody(uint16_t tag, uint16_t ch, uint16_t bits, uint32_t rate, uint32_t size = 16) {
    std::vector<uint8_t> b;
    test::putTag(b, "fmt ");
    test::putU32(b, size);
    test::putU16(b, tag); test::putU16(b, ch); test::putU32(b, rate);
    test::putU32(b, rate * ch * bits / 8); test::putU16(b, ch * bits / 8); test::putU16(b, bits);
    while (b.size() < 8 + size) b.push_back(0);
    return b;
}
void dataBody(std::vector<uint8_t>& b, uint32_t frames, uint16_t ch) {
    test::putTag(b, "data");
    test::putU32(b, frames * ch * 2);
    for (uint32_t i = 0; i < frames * ch; ++i) test::putU16(b, static_cast<uint16_t>(i * 37));
}
WavLayout layoutOrEmpty(const std::string& p, bool& ok) {
    WavLayout l;
    FILE* f = std::fopen(p.c_str(), "rb");
    ok = f && l.parse(f);
    if (f) std::fclose(f);
    return l;
}
} // namespace

TEST_CASE("the converter accepts exactly the files the engine plays", "[KitRateConvert][review2]") {
    freshRoot();
    bool ok = false;
    // The engine plays any 16-bit data whatever the format tag says.
    auto b = fmtBody(3, 2, 16, 48000);
    dataBody(b, 100, 2);
    test::writeRiff(g_root + "/tag3.wav", b);
    WavLayout l = layoutOrEmpty(g_root + "/tag3.wav", ok);
    CHECK(ok);
    CHECK(l.pcm16());
    // The engine refuses a data chunk before its fmt.
    std::vector<uint8_t> late;
    dataBody(late, 100, 1);
    auto f1 = fmtBody(1, 1, 16, 48000);
    late.insert(late.end(), f1.begin(), f1.end());
    test::writeRiff(g_root + "/late.wav", late);
    layoutOrEmpty(g_root + "/late.wav", ok);
    CHECK_FALSE(ok);
    // ... a fmt chunk too short to say what it is ...
    auto shortFmt = fmtBody(1, 1, 16, 48000, 14);
    dataBody(shortFmt, 100, 1);
    test::writeRiff(g_root + "/short.wav", shortFmt);
    layoutOrEmpty(g_root + "/short.wav", ok);
    CHECK_FALSE(ok);
    // ... and data behind more than 64 chunks.
    auto many = fmtBody(1, 1, 16, 48000);
    for (int i = 0; i < 70; ++i) { test::putTag(many, "junk"); test::putU32(many, 0); }
    dataBody(many, 100, 1);
    test::writeRiff(g_root + "/many.wav", many);
    layoutOrEmpty(g_root + "/many.wav", ok);
    CHECK_FALSE(ok);
}

TEST_CASE("a sample named with ./ or // is the kit's only in its plain spelling", "[KitRateConvert][review2]") {
    // FAT on the board opens neither "." nor ".." (FF_FS_RPATH 0): such a
    // spelling is not a file of the kit the tidy may remove.
    freshRoot();
    const std::string kit = g_root + "/kits/kitA";
    writeWav(kit + "/a.wav", 1, 48000, 4800);
    writeText(kit + "/kit.json", "{\"pads\":[{\"samples\":[{\"sample\":\"./a.wav\"}]}]}");
    TestHost host;
    KitRateConverter c;
    REQUIRE(c.begin(host, kit + "/kit.json", 44100));
    REQUIRE(runAll(c) == KitRateConverter::Phase::Done);
    CHECK(parseKit(kit + "/kit.json").pads[0].samples[0].sample == "44k1/_shared/a.wav");
    tidyKit(host, kit + "/kit.json", g_root + "/kits", 44100);
    CHECK(fs::exists(kit + "/a.wav"));
}

TEST_CASE("the tidy keeps a file any kit can still reach, however it spells it", "[KitRateConvert][review2]") {
    const char* refs[] = {
        "../kitA/./kick.wav",          // a dot segment
        "../kitA//kick.wav",           // a doubled separator
        "../KITA/KICK.WAV",            // FAT ignores case
    };
    for (const char* ref : refs) {
        freshRoot();
        const std::string json = makeKitA();
        const std::string kit = g_root + "/kits/kitA";
        writeText(g_root + "/kits/kitB/kit.json",
                  std::string("{\"pads\":[{\"samples\":[{\"sample\":\"") + ref + "\"}]}]}");
        TestHost host;
        KitRateConverter c;
        REQUIRE(c.begin(host, json, 44100));
        REQUIRE(runAll(c) == KitRateConverter::Phase::Done);
        tidyKit(host, json, g_root + "/kits", 44100);
        INFO(ref);
        CHECK(fs::exists(kit + "/kick.wav"));
    }
}

TEST_CASE("the tidy keeps everything when a kit cannot be read or named", "[KitRateConvert][review2]") {
    // A pitched kit's zone, a kit saved only as .bak, a JSON too large to
    // search: each one names kick.wav, and each must keep it.
    for (int variant = 0; variant < 3; ++variant) {
        freshRoot();
        const std::string json = makeKitA();
        const std::string kit = g_root + "/kits/kitA";
        const std::string ref = g_root + "/kits/kitA/kick.wav";
        if (variant == 0) {
            writeText(g_root + "/kits/p/kit.json",
                      "{\"engine\":\"pitched\",\"instrument\":{\"zones\":[{\"sample\":\"" + ref + "\"}]},\"pads\":[]}");
        } else if (variant == 1) {
            writeText(g_root + "/kits/b/kit.json.bak",
                      "{\"pads\":[{\"samples\":[{\"sample\":\"" + ref + "\"}]}]}");
        } else {
            std::string big = "{\"pads\":[{\"samples\":[{\"sample\":\"" + ref + "\"}]}],\"pad\":\"";
            big.append(300 * 1024, 'x');
            big += "\"}";
            writeText(g_root + "/kits/big/kit.json", big);
        }
            TestHost host;
        KitRateConverter c;
        REQUIRE(c.begin(host, json, 44100));
        REQUIRE(runAll(c) == KitRateConverter::Phase::Done);
        tidyKit(host, json, g_root + "/kits", 44100);
        INFO("variant " << variant);
        CHECK(fs::exists(kit + "/kick.wav"));
    }
}

TEST_CASE("a kit whose folder leaves no room for the rate folder is refused", "[KitRateConvert][review2]") {
    // kitDir + "/44k1/.originals" past the path buffer: the folder's name was
    // never written and garbage was compared and written to.
    freshRoot();
    // The kit's JSON and a converted "a.wav" fit the 192-byte buffers; the
    // tidy's list, "<kit>/44k1/.originals", does not.
    const size_t kitDirLen = 178;
    const std::string name(kitDirLen - g_root.size() - std::strlen("/kits/"), 'k');
    const std::string kit = g_root + "/kits/" + name;
    writeWav(kit + "/a.wav", 1, 48000, 480);
    writeText(kit + "/kit.json", "{\"pads\":[{\"samples\":[{\"sample\":\"a.wav\"}]}]}");
    TestHost host;
    KitRateConverter c;
    if (c.begin(host, kit + "/kit.json", 44100)) runAll(c);
    CHECK(c.phase() == KitRateConverter::Phase::Failed);
    CHECK(std::string(c.progress().error) == "path too long");
    CHECK_FALSE(fs::exists(kit + "/44k1"));
}

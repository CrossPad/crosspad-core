// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "crosspad/dsp/FftResampler.hpp"
#include "crosspad/dsp/PortableFft.hpp"
#include "crosspad/instrument/PitchedKitLoader.hpp"
#include "../src/kit/KitJsonParser.hpp"
#include "wav_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <system_error>
#include <vector>

using namespace crosspad;

namespace {

void* loadAlloc(size_t n) { return std::malloc(n); }
void  loadFree(void* p)   { std::free(p); }

/// The allocator, watched: what the loader asks for is the claim under test in
/// the streaming cases — a zone is never held twice, whatever its length.
struct AllocWatch {
    size_t largest = 0;
    size_t live = 0;
    size_t peak = 0;
    std::map<void*, size_t> blocks;
};
AllocWatch g_alloc;

void* watchAlloc(size_t n) {
    void* p = std::malloc(n);
    if (!p) return nullptr;
    g_alloc.largest = std::max(g_alloc.largest, n);
    g_alloc.blocks[p] = n;
    g_alloc.live += n;
    g_alloc.peak = std::max(g_alloc.peak, g_alloc.live);
    return p;
}

void watchFree(void* p) {
    auto it = g_alloc.blocks.find(p);
    if (it != g_alloc.blocks.end()) {
        g_alloc.live -= it->second;
        g_alloc.blocks.erase(it);
    }
    std::free(p);
}

/// A kit of exactly one zone, rooted where the caller says.
KitInfo oneZoneKit(const char* sample, float root) {
    KitInfo kit;
    kit.name = "ONE ZONE";
    kit.path = "/kits/one/kit.json";
    kit.engine = KitEngine::Pitched;
    InstrumentZoneSpec z;
    z.sample = sample;
    z.root = root;
    kit.instrument.zones = {z};
    return kit;
}

std::vector<std::string> g_log;
void captureLog(const char* line) { g_log.push_back(line); }

/// The loader is handed logical names; the test's "mount" is a temp directory.
std::string g_root;
std::string testResolve(const std::string& kitPath, const std::string& sample) {
    (void)kitPath;
    return g_root + "/" + sample;
}

/// One temp kit directory per test run: a3.wav at 44.1 kHz (A3, 220 Hz) and
/// a4.wav at 48 kHz (A4, 440 Hz) so one zone exercises rate conversion.
struct KitDir {
    KitDir() {
        /* Per process: ctest -j runs the cases side by side, and a shared
         * directory was removed under the one still reading it. */
        g_root = (std::filesystem::temp_directory_path() /
                  ("cp_pitched_kit_" + std::to_string(std::random_device{}()) + "_" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
                     .string();
        std::filesystem::remove_all(g_root);
        std::filesystem::create_directories(g_root);
        test::writeSineWav(g_root + "/a3.wav", 220.0, 44100, 0.5);
        test::writeSineWav(g_root + "/a4.wav", 440.0, 48000, 0.5);
        g_log.clear();
    }
    ~KitDir() {
        std::error_code ec;
        std::filesystem::remove_all(g_root, ec);
    }
};

PitchedLoadEnv makeEnv(size_t maxBankBytes = 0) {
    PitchedLoadEnv env;
    env.alloc = loadAlloc;
    env.free = loadFree;
    env.engineRate = 44100;
    env.resolvePath = testResolve;
    env.log = captureLog;
    env.maxBankBytes = maxBankBytes;
    return env;
}

/// A pitched kit built directly, the way a parser would leave it: zone 0 with
/// an explicit root, zone 1 at 48 kHz asking for detection.
KitInfo makeKit() {
    KitInfo kit;
    kit.name = "SINE PITCHED";
    kit.path = "/kits/sine/kit.json";
    kit.engine = KitEngine::Pitched;

    InstrumentZoneSpec a3;
    a3.sample = "a3.wav";
    a3.root = 57.0f;                      // A3
    InstrumentZoneSpec a4;
    a4.sample = "a4.wav";
    a4.root = -1.0f;                      // "auto"
    kit.instrument.zones = {a3, a4};

    for (uint8_t p = 0; p < KIT_PADS; ++p) kit.instrument.padNotes[p] = 60 + p;
    kit.instrument.padNotesSet = true;
    return kit;
}

bool logContains(const char* needle) {
    for (const auto& l : g_log) {
        if (l.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

TEST_CASE("pitchedBuildBank: builds both zones, converting the 48 kHz one",
          "[instrument][loader]") {
    KitDir dir;
    PortableFft fft;
    const KitInfo kit = makeKit();
    PitchedLoadEnv env = makeEnv();

    SampleBank* bank = nullptr;
    char err[128] = {'x', 0};
    REQUIRE(pitchedBuildBank(kit, env, fft, bank, err, sizeof(err)));
    REQUIRE(bank != nullptr);
    REQUIRE(err[0] == '\0');
    REQUIRE(bank->zoneCount() == 2);

    // Zone 0 is already at the engine rate: length untouched, root as written.
    REQUIRE(bank->zone(0).length == 22050);
    REQUIRE(bank->zone(0).rootMidi == Catch::Approx(57.0f));

    // Zone 1 came in at 48 kHz: resampled to the engine rate, and its root
    // (A4 = 440 Hz = MIDI 69) found by the detector.
    REQUIRE(bank->zone(1).length == resampledLength(24000, 48000, 44100));
    REQUIRE(bank->zone(1).length == 22050);
    REQUIRE(std::fabs(bank->zone(1).rootMidi - 69.0f) < 0.05f);

    REQUIRE(bank->bytes() == (22050u + 22050u) * sizeof(int16_t));

    // Both roots are accounted for in the log, and each says where it came from.
    REQUIRE(logContains("(from kit.json)"));
    REQUIRE(logContains("detected"));
    REQUIRE(logContains("48000 Hz -> 44100 Hz"));

    // nearestZone picks per root, which is the whole point of two zones.
    REQUIRE(bank->nearestZone(58.0f) == &bank->zone(0));
    REQUIRE(bank->nearestZone(70.0f) == &bank->zone(1));

    delete bank;
}

TEST_CASE("pitchedBuildBank: a zone over budget is truncated, not refused",
          "[instrument][loader]") {
    KitDir dir;
    PortableFft fft;
    const KitInfo kit = makeKit();
    // Zone 0 alone is 44100 B; this admits it whole and leaves 7950 frames for
    // zone 1, which wants 22050.
    PitchedLoadEnv env = makeEnv(60000);

    SampleBank* bank = nullptr;
    char err[128] = {'x', 0};
    PitchedLoadReport report;
    REQUIRE(pitchedBuildBank(kit, env, fft, bank, err, sizeof(err), &report));
    REQUIRE(bank != nullptr);
    REQUIRE(err[0] == '\0');
    REQUIRE(bank->zoneCount() == 2);
    REQUIRE(bank->zone(0).length == 22050);
    REQUIRE(bank->zone(1).length == 7950);
    REQUIRE(bank->bytes() <= 60000u);

    REQUIRE(report.zones == 2);
    REQUIRE(report.truncatedZones == 1u);
    REQUIRE(report.droppedSeconds == Catch::Approx((22050.0 - 7950.0) / 44100.0).margin(0.01));
    INFO("warning = " << report.warning);
    REQUIRE(std::string(report.warning).find("zone 1 'a4.wav'") != std::string::npos);
    REQUIRE(std::string(report.warning).find("cut to") != std::string::npos);
    REQUIRE(std::string(report.warning).find("bank budget") != std::string::npos);
    REQUIRE(logContains("cut to"));

    delete bank;
}

TEST_CASE("pitchedBuildBank: a long zone is cut to the budget, loop and all",
          "[instrument][loader]") {
    KitDir dir;
    PortableFft fft;
    test::writeSineWav(g_root + "/long.wav", 220.0, 44100, 3.0);

    KitInfo kit = oneZoneKit("long.wav", 57.0f);
    kit.instrument.zones[0].loopStart = 1000;
    kit.instrument.zones[0].loopEnd = 100000;        // inside the file, past the budget

    const uint32_t budgetFrames = 44100;             // one second
    PitchedLoadEnv env = makeEnv(budgetFrames * sizeof(int16_t));

    SampleBank* bank = nullptr;
    char err[128] = {0};
    PitchedLoadReport report;
    INFO("err = " << err);
    REQUIRE(pitchedBuildBank(kit, env, fft, bank, err, sizeof(err), &report));
    REQUIRE(bank->zoneCount() == 1);
    REQUIRE(bank->zone(0).length == budgetFrames);
    REQUIRE(bank->zone(0).loopStart == 1000);
    REQUIRE(bank->zone(0).loopEnd == budgetFrames);  // clamped with the audio it named
    REQUIRE(report.truncatedZones == 1u);
    REQUIRE(report.droppedSeconds == Catch::Approx(2.0).margin(0.01));
    INFO("warning = " << report.warning);
    REQUIRE(std::string(report.warning).find("3.0 s cut to 1.0 s") != std::string::npos);

    // The kept second is the file's first second, not silence.
    REQUIRE(bank->zone(0).pcm[0] == 0);
    int32_t peak = 0;
    for (uint32_t i = 0; i < budgetFrames; ++i) {
        peak = std::max<int32_t>(peak, std::abs(bank->zone(0).pcm[i]));
    }
    REQUIRE(peak > 19000);
    delete bank;
}

TEST_CASE("pitchedBuildBank: a 48 kHz zone is truncated at the output rate",
          "[instrument][loader]") {
    KitDir dir;
    PortableFft fft;
    test::writeSineWav(g_root + "/long48.wav", 440.0, 48000, 1.0);

    // 48000 source frames become 44100; the budget admits 20000 of those.
    const uint32_t budgetFrames = 20000;
    KitInfo kit = oneZoneKit("long48.wav", -1.0f);   // and detect the root from what is kept
    PitchedLoadEnv env = makeEnv(budgetFrames * sizeof(int16_t));

    SampleBank* bank = nullptr;
    char err[128] = {0};
    PitchedLoadReport report;
    INFO("err = " << err);
    REQUIRE(pitchedBuildBank(kit, env, fft, bank, err, sizeof(err), &report));
    REQUIRE(bank->zone(0).length == budgetFrames);
    REQUIRE(report.truncatedZones == 1u);
    REQUIRE(report.droppedSeconds
            == Catch::Approx((44100.0 - budgetFrames) / 44100.0).margin(0.01));

    // Converted audio, not a raw copy of the 48 kHz samples: A4 at the engine
    // rate is what the detector has to find in it.
    REQUIRE(std::fabs(bank->zone(0).rootMidi - 69.0f) < 0.05f);
    delete bank;
}

TEST_CASE("pitchedBuildBank: a 30 s zone never costs more than a block of scratch",
          "[instrument][loader]") {
    KitDir dir;
    PortableFft fft;
    test::writeSineWav(g_root + "/huge48.wav", 440.0, 48000, 30.0);

    const uint32_t budgetFrames = 200000;            // ~4.5 s of the ~27.6 s it converts to
    const size_t budgetBytes = budgetFrames * sizeof(int16_t);
    KitInfo kit = oneZoneKit("huge48.wav", 69.0f);

    PitchedLoadEnv env = makeEnv(budgetBytes);
    env.alloc = watchAlloc;
    env.free = watchFree;
    g_alloc = AllocWatch{};

    SampleBank* bank = nullptr;
    char err[128] = {0};
    PitchedLoadReport report;
    INFO("err = " << err);
    REQUIRE(pitchedBuildBank(kit, env, fft, bank, err, sizeof(err), &report));
    REQUIRE(bank->zone(0).length == budgetFrames);
    REQUIRE(report.truncatedZones == 1u);

    // The zone itself is the biggest thing ever asked for: no request holds a
    // copy of the file, and the transient scratch is one resampler block.
    const size_t kScratchCeiling = 512u * 1024u;
    INFO("largest request = " << g_alloc.largest << " B, peak live = " << g_alloc.peak << " B");
    REQUIRE(g_alloc.largest <= budgetBytes);
    REQUIRE(g_alloc.largest < kScratchCeiling);
    REQUIRE(g_alloc.peak < budgetBytes + kScratchCeiling);

    delete bank;
    REQUIRE(g_alloc.live == 0u);
}

TEST_CASE("pitchedBuildBank: a zone with no budget left at all is refused by name",
          "[instrument][loader]") {
    KitDir dir;
    PortableFft fft;
    const KitInfo kit = makeKit();
    PitchedLoadEnv env = makeEnv(1);                 // not even one frame

    SampleBank* bank = nullptr;
    char err[128] = {0};
    REQUIRE_FALSE(pitchedBuildBank(kit, env, fft, bank, err, sizeof(err), nullptr));
    REQUIRE(bank == nullptr);
    INFO("err = " << err);
    REQUIRE(std::string(err).find("zone 0") != std::string::npos);
    REQUIRE(std::string(err).find("budget") != std::string::npos);
}

TEST_CASE("pitchedBuildBank: a missing sample names the zone and the file",
          "[instrument][loader]") {
    KitDir dir;
    PortableFft fft;
    KitInfo kit = makeKit();
    kit.instrument.zones[1].sample = "nope.wav";
    PitchedLoadEnv env = makeEnv();

    SampleBank* bank = nullptr;
    char err[128] = {0};
    REQUIRE_FALSE(pitchedBuildBank(kit, env, fft, bank, err, sizeof(err)));
    REQUIRE(bank == nullptr);
    INFO("err = " << err);
    REQUIRE(std::string(err).find("zone 1") != std::string::npos);
    REQUIRE(std::string(err).find("nope.wav") != std::string::npos);
}

TEST_CASE("pitchedBuildBank: refuses an env it cannot work with",
          "[instrument][loader]") {
    KitDir dir;
    PortableFft fft;
    const KitInfo kit = makeKit();
    SampleBank* bank = nullptr;
    char err[128] = {0};

    PitchedLoadEnv noAlloc = makeEnv();
    noAlloc.alloc = nullptr;
    REQUIRE_FALSE(pitchedBuildBank(kit, noAlloc, fft, bank, err, sizeof(err)));
    REQUIRE(std::string(err) == "no allocator");

    PitchedLoadEnv noRate = makeEnv();
    noRate.engineRate = 0;
    REQUIRE_FALSE(pitchedBuildBank(kit, noRate, fft, bank, err, sizeof(err)));
    REQUIRE(std::string(err) == "engine rate 0");

    KitInfo empty = kit;
    empty.instrument.zones.clear();
    PitchedLoadEnv env = makeEnv();
    REQUIRE_FALSE(pitchedBuildBank(empty, env, fft, bank, err, sizeof(err)));
    REQUIRE(std::string(err) == "no zones");

    REQUIRE(bank == nullptr);
}

TEST_CASE("pitchedBuildBank: loads the kit a kit.json parse produces",
          "[instrument][loader]") {
    KitDir dir;
    PortableFft fft;

    // The round trip that matters: what kit_parse_* writes into KitInfo is
    // exactly what the loader consumes — "auto" as a root included.
    const char* json = R"({
      "name": "SINE PITCHED", "author": "test", "engine": "pitched", "type": 0,
      "instrument": {
        "zones": [
          { "sample": "a3.wav", "root": 57 },
          { "sample": "a4.wav", "root": "auto" }
        ],
        "pad_notes": [60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75],
        "release_ms": 150, "attack_ms": 2, "max_voices": 24
      }
    })";

    JsonDocument doc;
    REQUIRE(deserializeJson(doc, json) == DeserializationError::Ok);
    KitInfo kit;
    REQUIRE(kit_parse_metadata(kit, doc));
    REQUIRE(kit.engine == KitEngine::Pitched);
    REQUIRE(kit.instrument.zones.size() == 2);
    REQUIRE(kit.instrument.noteForPad(15) == 75);

    PitchedLoadEnv env = makeEnv();
    SampleBank* bank = nullptr;
    char err[128] = {0};
    REQUIRE(pitchedBuildBank(kit, env, fft, bank, err, sizeof(err)));
    REQUIRE(bank != nullptr);
    REQUIRE(bank->zoneCount() == 2);
    REQUIRE(bank->zone(0).rootMidi == Catch::Approx(57.0f));
    REQUIRE(std::fabs(bank->zone(1).rootMidi - 69.0f) < 0.05f);
    delete bank;
}

TEST_CASE("pitchedBuildBank: a null resolver falls back to the kit path helper",
          "[instrument][loader]") {
    KitDir dir;
    PortableFft fft;
    KitInfo kit = makeKit();
    kit.path = g_root + "/kit.json";      // the zones sit beside it, flat

    PitchedLoadEnv env = makeEnv();
    env.resolvePath = nullptr;

    SampleBank* bank = nullptr;
    char err[128] = {0};
    INFO("err = " << err);
    REQUIRE(pitchedBuildBank(kit, env, fft, bank, err, sizeof(err)));
    REQUIRE(bank != nullptr);
    REQUIRE(bank->zoneCount() == 2);
    delete bank;
}

namespace {
int g_yields = 0;
void countYield() { ++g_yields; }
} // namespace

TEST_CASE("pitchedBuildBank: a zone longer than the zone limit is cut, and says why",
          "[instrument][loader]") {
    KitDir dir;
    PortableFft fft;
    test::writeSineWav(g_root + "/take.wav", 220.0, 44100, 3.0);
    const KitInfo kit = oneZoneKit("take.wav", 57.0f);

    PitchedLoadEnv env = makeEnv();
    env.maxZoneFrames = 44100;                       // one second
    SampleBank* bank = nullptr;
    char err[128] = {0};
    PitchedLoadReport report;
    REQUIRE(pitchedBuildBank(kit, env, fft, bank, err, sizeof(err), &report));
    REQUIRE(bank->zone(0).length == 44100u);
    REQUIRE(report.truncatedZones == 1u);
    INFO("warning = " << report.warning);
    REQUIRE(std::string(report.warning).find("3.0 s cut to 1.0 s (zone limit)") != std::string::npos);
    delete bank;
}

TEST_CASE("pitchedBuildBank: yields between blocks, converting or not", "[instrument][loader]") {
    KitDir dir;
    PortableFft fft;
    test::writeSineWav(g_root + "/long44.wav", 220.0, 44100, 2.0);
    test::writeSineWav(g_root + "/long48.wav", 220.0, 48000, 2.0);

    for (const char* file : {"long44.wav", "long48.wav"}) {
        PitchedLoadEnv env = makeEnv();
        env.yield = countYield;
        g_yields = 0;
        SampleBank* bank = nullptr;
        char err[128] = {0};
        INFO("file = " << file);
        REQUIRE(pitchedBuildBank(oneZoneKit(file, 57.0f), env, fft, bank, err, sizeof(err)));
        REQUIRE(g_yields >= 10);                     // 2 s in blocks of 4096 frames
        delete bank;
    }
}

TEST_CASE("pitchedBuildBank: without rate conversion a zone keeps its own rate", "[instrument][loader]") {
    KitDir dir;
    PortableFft fft;
    PitchedLoadEnv env = makeEnv();
    env.convertRate = false;                          // engine 44.1 kHz, a4.wav 48 kHz
    SampleBank* bank = nullptr;
    char err[128] = {0};
    REQUIRE(pitchedBuildBank(oneZoneKit("a4.wav", -1.0f), env, fft, bank, err, sizeof(err)));
    INFO("err = " << err);
    REQUIRE(bank->zone(0).length == 24000u);          // 0.5 s as written, not 22050
    REQUIRE(bank->zone(0).rate == Catch::Approx(48000.0 / 44100.0));
    // Detected at the file's rate: 440 Hz is A4 whatever the engine runs at.
    REQUIRE(bank->zone(0).rootMidi == Catch::Approx(69.0).margin(0.1));
    REQUIRE_FALSE(logContains("Hz ->"));             // nothing was converted
    delete bank;
}

TEST_CASE("pitchedBuildBank: normalize brings a zone's peak to -1 dBFS", "[instrument][loader]") {
    KitDir dir;
    PortableFft fft;
    PitchedLoadEnv env = makeEnv();
    SampleBank* bank = nullptr;
    char err[128] = {0};
    REQUIRE(pitchedBuildBank(oneZoneKit("a3.wav", 57.0f), env, fft, bank, err, sizeof(err)));
    REQUIRE(bank->zone(0).gain == 1.0f);              // off: as recorded
    delete bank;

    env.normalize = true;                             // the fixture peaks at 20000
    bank = nullptr;
    REQUIRE(pitchedBuildBank(oneZoneKit("a3.wav", 57.0f), env, fft, bank, err, sizeof(err)));
    REQUIRE(bank->zone(0).gain == Catch::Approx(kNormalizePeak * 32767.0 / 20000.0).epsilon(0.01));
    delete bank;
}

// ---------------------------------------------------------------------------
// The memory generation (PitchedLoadEnv::generation).
//
// The bank is built out of memory that can be wiped wholesale between two
// allocations (the device's engine arena reset). A build that saw such a wipe
// holds zones whose PCM is gone: it must fail, and must not hand those blocks
// back to the heap that replaced them.
// ---------------------------------------------------------------------------

namespace {
uint32_t g_gen = 0;
uint32_t genNow() { return g_gen; }
int g_wipeYields = 0;
/// The wipe, in the middle of the build: after the first zone is read.
void wipeOnYield() {
    if (++g_wipeYields == 1) ++g_gen;
}
}

TEST_CASE("pitchedBuildBank: the bank carries the env's memory generation",
          "[instrument][loader][generation]") {
    KitDir dir;
    PortableFft fft;
    g_gen = 41;
    PitchedLoadEnv env = makeEnv();
    env.generation = genNow;
    SampleBank* bank = nullptr;
    char err[128] = {0};
    REQUIRE(pitchedBuildBank(makeKit(), env, fft, bank, err, sizeof(err)));
    REQUIRE(bank != nullptr);
    REQUIRE(bank->generation() == 41);
    REQUIRE_FALSE(bank->stale());
    delete bank;
}

TEST_CASE("pitchedBuildBank: a wipe during the build fails it and frees nothing stale",
          "[instrument][loader][generation]") {
    KitDir dir;
    PortableFft fft;
    g_gen = 5;
    g_wipeYields = 0;
    g_alloc = AllocWatch{};
    PitchedLoadEnv env = makeEnv();
    env.alloc = watchAlloc;
    env.free = watchFree;
    env.generation = genNow;
    env.yield = wipeOnYield;
    SampleBank* bank = nullptr;
    char err[128] = {0};
    REQUIRE_FALSE(pitchedBuildBank(makeKit(), env, fft, bank, err, sizeof(err)));
    REQUIRE(bank == nullptr);
    REQUIRE(std::string(err).find("memory reset during the build") != std::string::npos);
    // The zone PCM taken before the wipe was forgotten, not freed: it is still
    // in the watch's books (the host's malloc outlives a "reset").
    REQUIRE_FALSE(g_alloc.blocks.empty());
    for (auto& b : g_alloc.blocks) std::free(b.first);
    g_alloc = AllocWatch{};
}

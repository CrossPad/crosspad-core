// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "crosspad/dsp/FftResampler.hpp"
#include "crosspad/dsp/PortableFft.hpp"
#include "crosspad/instrument/PitchedKitLoader.hpp"
#include "../src/kit/KitJsonParser.hpp"
#include "wav_fixture.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace crosspad;

namespace {

void* loadAlloc(size_t n) { return std::malloc(n); }
void  loadFree(void* p)   { std::free(p); }

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
        g_root = (std::filesystem::temp_directory_path() / "cp_pitched_kit").string();
        std::filesystem::remove_all(g_root);
        std::filesystem::create_directories(g_root);
        test::writeSineWav(g_root + "/a3.wav", 220.0, 44100, 0.5);
        test::writeSineWav(g_root + "/a4.wav", 440.0, 48000, 0.5);
        g_log.clear();
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

TEST_CASE("pitchedBuildBank: a bank over budget is refused by name",
          "[instrument][loader]") {
    KitDir dir;
    PortableFft fft;
    const KitInfo kit = makeKit();
    // Zone 0 alone is 44100 B; this admits it and refuses zone 1.
    PitchedLoadEnv env = makeEnv(60000);

    SampleBank* bank = nullptr;
    char err[128] = {0};
    REQUIRE_FALSE(pitchedBuildBank(kit, env, fft, bank, err, sizeof(err)));
    REQUIRE(bank == nullptr);          // the partial bank was freed, not handed back
    INFO("err = " << err);
    REQUIRE(std::string(err).find("budget") != std::string::npos);
    REQUIRE(std::string(err).find("zone 1") != std::string::npos);
    REQUIRE(std::string(err).find("60000") != std::string::npos);
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

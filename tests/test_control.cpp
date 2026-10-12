// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "crosspad/control/KitStatus.hpp"
#include "crosspad/control/LedState.hpp"
#include "crosspad/control/MixerLevels.hpp"
#include "crosspad/control/PitchedStatus.hpp"
#include "crosspad/control/WaveStatus.hpp"
#include "crosspad/instrument/PitchedInstrument.hpp"
#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "test_helpers.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace crosspad;
using namespace crosspad::control;

namespace {

class FakeKitManager : public IKitManager {
public:
    std::vector<KitInfo> kits;
    int current = -1;

    int getKitCount() const override { return (int)kits.size(); }
    KitInfo* getKit(int id) override { return id >= 0 && id < (int)kits.size() ? &kits[id] : nullptr; }
    KitInfo* getCurrentKit() override { return getKit(current); }
    int getCurrentKitId() const override { return current; }
    void setCurrentKitId(int id) override { current = id; }
    bool isLoading() const override { return false; }
    std::vector<KitInfo> listKits(const std::string&) override { return kits; }
    KitLoadResult loadKit(KitInfo&) override { return {}; }
    bool saveKit(const KitInfo&) override { return false; }
};

/// Registers @p mgr for the scope, and takes it back out.
struct KitManagerScope {
    explicit KitManagerScope(IKitManager* mgr) { getPlatformServices().kitManager = mgr; }
    ~KitManagerScope() { getPlatformServices().kitManager = nullptr; }
};

struct FakeMixer {
    struct Ch { bool active; const char* name; float l, r, vol; bool mute; uint32_t routes; };
    struct Out { float l, r, vol; bool mute; };
    std::vector<Ch> ch;
    std::vector<Out> out;

    uint8_t maxChannels() const { return (uint8_t)ch.size(); }
    uint8_t numOutputs() const { return (uint8_t)out.size(); }
    bool isChannelActive(uint8_t c) const { return ch[c].active; }
    void getChannelLevel(uint8_t c, float& l, float& r) const { l = ch[c].l; r = ch[c].r; }
    bool isRouteEnabled(uint8_t c, uint8_t o) const { return (ch[c].routes >> o) & 1u; }
    const char* getChannelName(uint8_t c) const { return ch[c].name; }
    float getChannelVolume(uint8_t c) const { return ch[c].vol; }
    bool isChannelMuted(uint8_t c) const { return ch[c].mute; }
    void getOutputLevel(uint8_t o, float& l, float& r) const { l = out[o].l; r = out[o].r; }
    float getOutputVolume(uint8_t o) const { return out[o].vol; }
    bool isOutputMuted(uint8_t o) const { return out[o].mute; }
};

std::vector<std::string> g_cached;
bool inCache(const char* path) {
    for (const auto& p : g_cached) if (p == path) return true;
    return false;
}

void* bankAlloc(size_t n) { return std::malloc(n); }
void  bankFree(void* p)   { std::free(p); }

} // namespace

TEST_CASE("control: kit status without a kit manager", "[control]") {
    KitManagerScope scope(nullptr);
    const KitStatus st = readKitStatus(true, 4);
    CHECK(st.count == 0);
    CHECK(st.current == -1);
    CHECK(st.name == nullptr);
    CHECK_FALSE(st.parsed);
    CHECK(st.loading);
    CHECK(st.pending == 4);
}

TEST_CASE("control: kit status names the current kit", "[control]") {
    FakeKitManager mgr;
    mgr.kits.resize(3);
    mgr.kits[1].name = "ŚWIĘTE SŁOWA";
    mgr.kits[1].parsed = true;
    KitManagerScope scope(&mgr);

    KitStatus st = readKitStatus(false, -1);
    CHECK(st.count == 3);
    CHECK(st.current == -1);
    CHECK(st.name == nullptr);

    mgr.current = 1;
    st = readKitStatus(false, -1);
    CHECK(st.current == 1);
    CHECK(std::string(st.name) == "ŚWIĘTE SŁOWA");
    CHECK(st.parsed);
    CHECK_FALSE(st.loading);
    CHECK(st.pending == -1);
}

TEST_CASE("control: LED state reads the controller and the saved brightness", "[control]") {
    test::MockLedStrip strip;
    SyncEventBus bus;
    PadLedController leds;
    leds.init(strip, bus, nullptr);
    leds.setPadColor(0, RgbColor(0x12, 0x34, 0x56));
    leds.setPadColor(15, RgbColor(0xFF, 0x00, 0x80));
    leds.setCoalescedRefresh(true);
    CrosspadSettings::getInstance()->RGBbrightness = 42;

    const LedState st = readLedState(leds);
    CHECK(st.brightness == leds.getBrightness());
    CHECK(st.animating == leds.getAnimating());
    CHECK(st.coalesced);
    CHECK(st.settingsBrightness == 42);
    CHECK(st.colors[0] == RgbColor(0x12, 0x34, 0x56));
    CHECK(st.colors[15] == RgbColor(0xFF, 0x00, 0x80));
    CHECK(st.colors[7] == leds.getPadColor(7));
}

TEST_CASE("control: mixer levels skip idle channels and carry the route mask", "[control]") {
    FakeMixer mx;
    mx.ch = {{true, "Sampler", 0.5f, 0.25f, 1.0f, false, 0b101},
             {false, "idle", 0.9f, 0.9f, 1.0f, false, 0},
             {true, nullptr, 0.0f, 0.0f, 0.8f, true, 0b010}};
    mx.out = {{0.1f, 0.2f, 1.0f, false}, {0.0f, 0.0f, 0.5f, true}, {0.3f, 0.3f, 0.7f, false}};

    std::vector<MixerChannelLevel> chans;
    std::vector<MixerOutputLevel> outs;
    readMixerLevels(mx, [&](const MixerChannelLevel& r) { chans.push_back(r); },
                    [&](const MixerOutputLevel& r) { outs.push_back(r); });

    REQUIRE(chans.size() == 2);
    CHECK(chans[0].ch == 0);
    CHECK(std::string(chans[0].name) == "Sampler");
    CHECK(chans[0].peakL == 0.5f);
    CHECK(chans[0].peakR == 0.25f);
    CHECK(chans[0].outputs == 3);
    CHECK(chans[0].routes == 0b101u);
    CHECK(chans[1].ch == 2);
    CHECK(chans[1].name == nullptr);
    CHECK(chans[1].muted);
    CHECK(chans[1].volume == 0.8f);
    CHECK(chans[1].routes == 0b010u);
    REQUIRE(outs.size() == 3);
    CHECK(outs[1].out == 1);
    CHECK(outs[1].muted);
    CHECK(outs[1].volume == 0.5f);
    CHECK(outs[2].peakL == 0.3f);
}

TEST_CASE("control: pitched status reads the engine and the bank apart", "[control]") {
    PitchedStatus st;
    st.active = true;
    readPitchedBank(nullptr, st);
    CHECK(st.active);                 // the port's to say, not the bank reader's
    CHECK(st.zones == 0);
    CHECK(st.bankBytes == 0);

    SampleBank bank(bankAlloc, bankFree);
    for (float root : {48.0f, 60.5f}) {
        Zone z; z.pcm = bank.allocPcm(100); z.length = 100; z.rootMidi = root;
        REQUIRE(bank.addZone(z));
    }
    readPitchedBank(&bank, st);
    CHECK(st.zones == 2);
    CHECK(st.roots[0] == 48.0f);
    CHECK(st.roots[1] == 60.5f);
    CHECK(st.roots[2] == 0.0f);
    CHECK(st.bankBytes == bank.bytes());

    PitchedInstrument inst;
    readPitchedEngine(inst, st);
    CHECK(st.steals == inst.stealCount());
    CHECK(st.sheds == inst.shedCount());
    CHECK(st.activeVoices == 0);
    CHECK(st.soundingVoices == 0);
    CHECK(st.maxVoices == inst.maxVoices());
    CHECK(st.voiceCap == inst.voiceCap());
    CHECK(st.lastNoteHz == 0.0f);
}

TEST_CASE("control: wave coverage lists the layers the cache lacks", "[control]") {
    FakeKitManager mgr;
    mgr.kits.resize(1);
    KitInfo& kit = mgr.kits[0];
    kit.path = "/sdcard/kits/Drums/kit.json";
    kit.pads[0].samples.push_back(SampleSlot(1, "kick.wav"));
    kit.pads[0].samples.push_back(SampleSlot(2, ""));          // an empty layer counts its index only
    kit.pads[0].samples.push_back(SampleSlot(3, "kick2.wav"));
    kit.pads[3].samples.push_back(SampleSlot(1, "/sdcard/shared/snare.wav"));
    kit.pads[5].samples.push_back(SampleSlot(1, "hat.wav"));
    mgr.current = 0;
    KitManagerScope scope(&mgr);
    g_cached = {"/sdcard/kits/Drums/kick.wav", "/sdcard/shared/snare.wav",
                "/sdcard/kits/Drums/SAMPLES/hat.wav"};

    WaveStatus st;
    readWaveCoverage(st, inCache);
    CHECK(st.kit == 0);
    CHECK(st.total == 4);
    CHECK(st.cached == 3);
    CHECK(std::string(st.missing) == "0:2");

    g_cached.clear();
    readWaveCoverage(st, inCache);
    CHECK(st.cached == 0);
    CHECK(std::string(st.missing) == "0:0,0:2,3:0,5:0");
}

TEST_CASE("control: wave coverage without a kit, and the loader counters", "[control]") {
    KitManagerScope scope(nullptr);
    WaveStatus st;
    std::strcpy(st.missing, "stale");
    st.total = 9;
    readWaveCoverage(st, inCache);
    CHECK(st.kit == -1);
    CHECK(st.total == 0);
    CHECK(st.missing[0] == '\0');

    struct { uint32_t requested = 1, dropped = 2, loaded = 3, retried = 4, failedInfo = 5,
             failedRead = 6, allocFail = 7, sweeps = 8, startFail = 9, queued = 10; } stats;
    setWaveLoader(st, stats, true);
    CHECK(st.loaderRunning);
    CHECK(st.requested == 1);
    CHECK(st.dropped == 2);
    CHECK(st.loaded == 3);
    CHECK(st.retried == 4);
    CHECK(st.failedInfo == 5);
    CHECK(st.failedRead == 6);
    CHECK(st.allocFail == 7);
    CHECK(st.sweeps == 8);
    CHECK(st.startFail == 9);
    CHECK(st.queued == 10);
}

TEST_CASE("control: the missing list stops where an entry would not fit", "[control]") {
    FakeKitManager mgr;
    mgr.kits.resize(1);
    for (int p = 0; p < KIT_PADS; ++p) {
        for (int l = 0; l < 20; ++l) mgr.kits[0].pads[p].samples.push_back(SampleSlot(l + 1, "x.wav"));
    }
    mgr.current = 0;
    KitManagerScope scope(&mgr);
    g_cached.clear();
    WaveStatus st;
    readWaveCoverage(st, inCache);
    CHECK(st.total == KIT_PADS * 20);
    const size_t len = std::strlen(st.missing);
    CHECK(len < WaveStatus::kMissingChars);
    CHECK(len >= WaveStatus::kMissingChars - 16);
    CHECK(st.missing[len - 1] != ',');
}

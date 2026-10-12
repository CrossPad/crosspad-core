// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "crosspad/control/AppName.hpp"
#include "crosspad/control/AudioLevel.hpp"
#include "crosspad/control/KitList.hpp"
#include "crosspad/control/KitStatus.hpp"
#include "crosspad/control/LedState.hpp"
#include "crosspad/control/MixerLevels.hpp"
#include "crosspad/control/PadInput.hpp"
#include "crosspad/control/PitchedStatus.hpp"
#include "crosspad/control/WaveStatus.hpp"
#include "crosspad/audio/IAudioModule.hpp"
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

class PadSpy : public IPadLogicHandler {
public:
    void onActivate(PadManager&) override {}
    void onDeactivate(PadManager&) override {}
    void onPadPress(PadManager&, uint8_t pad, uint8_t vel) override { log += "press " + std::to_string(pad) + " " + std::to_string(vel) + ";"; }
    void onPadRelease(PadManager&, uint8_t pad) override { log += "release " + std::to_string(pad) + ";"; }
    void onPadPressure(PadManager&, uint8_t pad, uint8_t p) override { log += "pressure " + std::to_string(pad) + " " + std::to_string(p) + ";"; }
    void onPadPan(PadManager&, uint8_t pad, int8_t pan) override { log += "pan " + std::to_string(pad) + " " + std::to_string(pan) + ";"; }
    std::string log;
};

/// Reports a fixed level per stream; everything else is inert.
class LevelModule : public IAudioModule {
public:
    bool setup(const AudioModuleConfig&) override { return true; }
    void process() override {}
    const AudioModuleConfig& getConfig() const override { return cfg_; }
    void addNode(IAudioNode*) override {}
    void removeNode(IAudioNode*) override {}
    size_t nodeCount() const override { return 0; }
    IAudioNode* getNode(size_t) const override { return nullptr; }
    IAudioStream* getOutputStream(uint8_t) override { return nullptr; }
    void getOutputLevel(uint8_t stream, float& l, float& r) const override {
        l = 0.25f * (float)(stream + 1);
        r = 0.125f;
    }
private:
    AudioModuleConfig cfg_;
};

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

TEST_CASE("control: kit list rows keep removed slots and say so", "[control]") {
    FakeKitManager mgr;
    mgr.kits.resize(3);
    mgr.kits[0].name = "A";
    mgr.kits[0].path = "/sdcard/kits/A/kit.json";
    mgr.kits[0].parsed = true;
    mgr.kits[1].name = "gone";            // removed: the slot stays, the path goes
    mgr.kits[2].name = "C";
    mgr.kits[2].path = "/sdcard/kits/C/kit.json";

    std::vector<KitListRow> rows;
    readKitList(mgr, [&](const KitListRow& r) { rows.push_back(r); });
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].id == 0);
    CHECK(std::string(rows[0].name) == "A");
    CHECK(rows[0].parsed);
    CHECK_FALSE(rows[0].removed);
    CHECK(rows[1].removed);
    CHECK(std::string(rows[1].name) == "gone");
    CHECK(rows[2].id == 2);
    CHECK_FALSE(rows[2].parsed);
}

TEST_CASE("control: a load request needs a kit manager and an id it holds", "[control]") {
    {
        KitManagerScope scope(nullptr);
        CHECK_FALSE(kitIdLoadable(0));
    }
    FakeKitManager mgr;
    mgr.kits.resize(2);
    KitManagerScope scope(&mgr);
    CHECK(kitIdLoadable(0));
    CHECK(kitIdLoadable(1));
    CHECK_FALSE(kitIdLoadable(2));
    CHECK_FALSE(kitIdLoadable(-1));
}

TEST_CASE("control: pad input reaches the active pad logic as the STM's would", "[control]") {
    test::CoreTestPlatform platform;
    REQUIRE(platform.init());
    auto spy = std::make_shared<PadSpy>();
    platform.padManager.registerPadLogic("Spy", spy);
    platform.padManager.setActivePadLogic("Spy");

    applyPadInput(platform.padManager, PadInput::Press, 3, 90);
    applyPadInput(platform.padManager, PadInput::Pressure, 3, 200);
    applyPadInput(platform.padManager, PadInput::Pan, 3, 127);
    applyPadInput(platform.padManager, PadInput::Pan, 3, 0);
    applyPadInput(platform.padManager, PadInput::Release, 3, 55);
    CHECK(spy->log == "press 3 90;pressure 3 200;pan 3 63;pan 3 -64;release 3;");
}

TEST_CASE("control: audio level of a stream", "[control]") {
    LevelModule module;
    getPlatformServices().setAudioModule(&module);
    const AudioLevel lv = readAudioLevel(1);
    getPlatformServices().setAudioModule(nullptr);
    CHECK(lv.stream == 1);
    CHECK(lv.left == 0.5f);
    CHECK(lv.right == 0.125f);
}

TEST_CASE("control: an app is named by its launcher name, case aside", "[control]") {
    CHECK(appNameMatches("Sampler", "sampler"));
    CHECK(appNameMatches("DAW Control", "daw control"));
    CHECK(appNameMatches("Mixer", "MIXER"));
    CHECK_FALSE(appNameMatches("Mixer", "Mix"));
    CHECK_FALSE(appNameMatches("Mix", "Mixer"));
    CHECK_FALSE(appNameMatches("Mixer", ""));
    CHECK_FALSE(appNameMatches(nullptr, "Mixer"));
    CHECK_FALSE(appNameMatches("Mixer", nullptr));
}

#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"
#include "crosspad/pad/IPadLogicHandler.hpp"

using namespace crosspad;

namespace {

// Dummy pad logic handler — just tracks activation for push/pop tests.
class DummyPadLogic : public IPadLogicHandler {
public:
    void onActivate(PadManager&) override { activateCount++; }
    void onDeactivate(PadManager&) override { deactivateCount++; }
    void onPadPress(PadManager&, uint8_t, uint8_t) override {}
    void onPadRelease(PadManager&, uint8_t) override {}
    void onPadPressure(PadManager&, uint8_t, uint8_t) override {}

    int activateCount = 0;
    int deactivateCount = 0;
};

} // namespace

TEST_CASE("PadManager: init via platform init", "[pad]") {
    test::CoreTestPlatform platform;
    REQUIRE(platform.init());
}

TEST_CASE("PadManager: pad press and release", "[pad]") {
    test::CoreTestPlatform platform;
    REQUIRE(platform.init());

    auto& pm = platform.padManager;

    SECTION("pad starts unpressed") {
        REQUIRE_FALSE(pm.isPadPressed(0));
    }

    SECTION("press sets state, release clears it") {
        pm.handlePadPress(0, 127);
        REQUIRE(pm.isPadPressed(0));

        pm.handlePadRelease(0);
        REQUIRE_FALSE(pm.isPadPressed(0));
    }

    SECTION("multiple pads are independent") {
        pm.handlePadPress(0, 100);
        pm.handlePadPress(3, 80);

        REQUIRE(pm.isPadPressed(0));
        REQUIRE(pm.isPadPressed(3));
        REQUIRE_FALSE(pm.isPadPressed(1));

        pm.handlePadRelease(0);
        REQUIRE_FALSE(pm.isPadPressed(0));
        REQUIRE(pm.isPadPressed(3));
    }
}

TEST_CASE("PadManager: note mapping", "[pad]") {
    test::CoreTestPlatform platform;
    REQUIRE(platform.init());

    auto& pm = platform.padManager;

    SECTION("set and get pad note") {
        pm.setPadNote(0, 60);
        REQUIRE(pm.getPadNote(0) == 60);

        pm.setPadNote(15, 72);
        REQUIRE(pm.getPadNote(15) == 72);
    }

    SECTION("set and get pad channel") {
        pm.setPadChannel(0, 5);
        REQUIRE(pm.getPadChannel(0) == 5);
    }
}

TEST_CASE("PadManager: input suppression", "[pad]") {
    test::CoreTestPlatform platform;
    REQUIRE(platform.init());

    auto& pm = platform.padManager;

    pm.setInputSuppressed(true);
    REQUIRE(pm.isInputSuppressed());

    pm.setInputSuppressed(false);
    REQUIRE_FALSE(pm.isInputSuppressed());
}

TEST_CASE("PadManager: LED color management", "[pad]") {
    test::CoreTestPlatform platform;
    REQUIRE(platform.init());

    auto& pm = platform.padManager;

    SECTION("set and get default color") {
        pm.setDefaultColor(255, 0, 128);
        auto color = pm.getPadColor(0);
        REQUIRE(color.R == 255);
        REQUIRE(color.G == 0);
        REQUIRE(color.B == 128);
    }
}

TEST_CASE("PadManager: brightness", "[pad]") {
    test::CoreTestPlatform platform;
    REQUIRE(platform.init());

    auto& pm = platform.padManager;

    pm.setBrightness(200);
    REQUIRE(pm.getBrightness() == 200);

    pm.setBrightness(0);
    REQUIRE(pm.getBrightness() == 0);

    pm.setBrightness(255);
    REQUIRE(pm.getBrightness() == 255);
}

TEST_CASE("PadManager: MIDI output on pad press", "[pad]") {
    test::CoreTestPlatform platform;
    REQUIRE(platform.init());

    test::RecordingMidiOutput midiOut;
    auto& services = getPlatformServices();
    services.setMidiOutput(&midiOut);

    auto* settings = CrosspadSettings::getInstance();
    settings->keypad.enableKeypad = true;
    settings->keypad.sendCC = false;

    auto& pm = platform.padManager;
    pm.handlePadPress(0, 100);
    pm.handlePadRelease(0);

    REQUIRE(midiOut.count() >= 2); // at least NoteOn + NoteOff

    // Cleanup
    settings->keypad.enableKeypad = true;
    services.setMidiOutput(nullptr);
}

TEST_CASE("PadManager: pushPadLogic/popPadLogic stack", "[pad]") {
    test::CoreTestPlatform platform;
    REQUIRE(platform.init());

    auto& pm = platform.padManager;

    auto a = std::make_shared<DummyPadLogic>();
    auto b = std::make_shared<DummyPadLogic>();
    pm.registerPadLogic("A", a);
    pm.registerPadLogic("B", b);

    pm.setActivePadLogic("A");
    REQUIRE(pm.getActivePadLogic() == "A");
    REQUIRE(a->activateCount == 1);

    pm.pushPadLogic("B");
    REQUIRE(pm.getActivePadLogic() == "B");
    REQUIRE(a->deactivateCount == 1); // pushing away from A deactivates it
    REQUIRE(b->activateCount == 1);

    pm.popPadLogic();
    REQUIRE(pm.getActivePadLogic() == "A");
    REQUIRE(b->deactivateCount == 1); // popping away from B deactivates it
    REQUIRE(a->activateCount == 2);   // restored A is re-activated

    // popPadLogic() on an empty stack is a no-op — no crash, state unchanged.
    pm.popPadLogic();
    REQUIRE(pm.getActivePadLogic() == "A");
    REQUIRE(a->activateCount == 2);
    REQUIRE(b->deactivateCount == 1);
}

namespace {

// Records which source each press and release reached the handler with.
class SourceSpy : public IPadLogicHandler {
public:
    void onActivate(PadManager&) override {}
    void onDeactivate(PadManager&) override {}
    void onPadPress(PadManager& pm, uint8_t pad, uint8_t) override { pressSeen = pm.pressSource(pad); }
    void onPadRelease(PadManager& pm, uint8_t pad) override { releaseSeen = pm.pressSource(pad); }
    void onPadPressure(PadManager&, uint8_t, uint8_t) override {}

    EventSource pressSeen   = EventSource::System;
    EventSource releaseSeen = EventSource::System;
};

} // namespace

TEST_CASE("PadManager: a handler can tell a played-back hit from a finger", "[pad]") {
    test::CoreTestPlatform platform;
    REQUIRE(platform.init());
    auto& pm = platform.padManager;
    auto spy = std::make_shared<SourceSpy>();
    pm.registerPadLogic("Spy", spy);
    pm.setActivePadLogic("Spy");

    pm.handlePadPress(2, 100, EventSource::SequencerCallback);
    REQUIRE(spy->pressSeen == EventSource::SequencerCallback);
    pm.handlePadRelease(2, EventSource::SequencerCallback);
    REQUIRE(spy->releaseSeen == EventSource::SequencerCallback);

    pm.handlePadPress(2, 100);
    REQUIRE(spy->pressSeen == EventSource::PadManager);
    pm.handlePadRelease(2);
    REQUIRE(spy->releaseSeen == EventSource::PadManager);
}

TEST_CASE("PadManager: the press source is per pad", "[pad]") {
    test::CoreTestPlatform platform;
    REQUIRE(platform.init());
    auto& pm = platform.padManager;
    pm.handlePadPress(1, 100, EventSource::SequencerCallback);
    pm.handlePadPress(5, 100);
    REQUIRE(pm.pressSource(1) == EventSource::SequencerCallback);
    REQUIRE(pm.pressSource(5) == EventSource::PadManager);
}

TEST_CASE("PadManager: getPadLogic finds a registered handler by name", "[pad]") {
    test::CoreTestPlatform platform;
    REQUIRE(platform.init());
    auto& pm = platform.padManager;
    auto spy = std::make_shared<SourceSpy>();
    pm.registerPadLogic("Spy", spy);
    REQUIRE(pm.getPadLogic("Spy") == spy);
    REQUIRE(pm.getPadLogic("Nobody") == nullptr);
}

TEST_CASE("PadManager: the input tap sees played pads, not the sequencer's", "[pad]") {
    test::CoreTestPlatform platform;
    REQUIRE(platform.init());
    auto& pm = platform.padManager;

    struct Seen { int count = 0; uint8_t pad = 0, vel = 0; } seen;
    pm.setInputTap([](void* ctx, uint8_t pad, uint8_t vel) {
        auto* s = static_cast<Seen*>(ctx);
        ++s->count;
        s->pad = pad;
        s->vel = vel;
    }, &seen);

    SECTION("a played press, also with app pad logic active") {
        pm.registerPadLogic("Dummy", std::make_shared<DummyPadLogic>());
        pm.setActivePadLogic("Dummy");
        pm.handlePadPress(5, 90);
        REQUIRE(seen.count == 1);
        CHECK(seen.pad == 5);
        CHECK(seen.vel == 90);
    }

    SECTION("the sequencer's playback is not tapped") {
        pm.handlePadPress(2, 100, EventSource::SequencerCallback);
        CHECK(seen.count == 0);
    }

    SECTION("an app's own playback (a kit's preview) is not tapped") {
        pm.handlePadPress(4, 100, EventSource::App);
        CHECK(seen.count == 0);
    }

    SECTION("a suppressed press is not tapped") {
        pm.setInputSuppressed(true);
        pm.handlePadPress(1, 100);
        CHECK(seen.count == 0);
    }
}

namespace {

// 16 levels of pitch on pad 6, for whatever is struck; reads back what the
// recorder said a played-back press was.
class LevelsLogic : public IPadLogicHandler {
public:
    void onActivate(PadManager&) override {}
    void onDeactivate(PadManager&) override {}
    void onPadPress(PadManager& pm, uint8_t pad, uint8_t) override { seen = pm.pressPerformance(pad); }
    void onPadRelease(PadManager&, uint8_t) override {}
    void onPadPressure(PadManager&, uint8_t, uint8_t) override {}
    PadPerformance performance(uint8_t, uint8_t& velocity) const override {
        velocity = 127;
        PadPerformance p;
        p.levels = PadLevels::Pitch;
        p.source = 6;
        return p;
    }
    PadPerformance seen;
};

} // namespace

TEST_CASE("PadManager: what a press would sound as comes from the active logic", "[pad][levels]") {
    test::CoreTestPlatform platform;
    REQUIRE(platform.init());
    auto& pm = platform.padManager;

    uint8_t vel = 30;
    REQUIRE(pm.performanceFor(3, vel).plain());     // the default logic has no mode
    REQUIRE(vel == 30);

    auto lv = std::make_shared<LevelsLogic>();
    pm.registerPadLogic("Levels", lv);
    pm.setActivePadLogic("Levels");
    const PadPerformance p = pm.performanceFor(3, vel);
    REQUIRE(p.levels == PadLevels::Pitch);
    REQUIRE(p.source == 6);
    REQUIRE(vel == 127);

    /* A played-back press carries what was recorded; a finger's is plain. */
    PadPerformance rec;
    rec.levels = PadLevels::Velocity;
    rec.source = 2;
    pm.handlePadPress(9, 55, EventSource::SequencerCallback, rec);
    REQUIRE(lv->seen.levels == PadLevels::Velocity);
    REQUIRE(lv->seen.source == 2);
    pm.handlePadPress(9, 55);
    REQUIRE(lv->seen.plain());
}

namespace {
/// Counts the strip lock: the audio render must never take it.
struct LockCountingStrip : test::MockLedStrip {
    void lock() override { ++locks; }
    int locks = 0;
};
} // namespace

TEST_CASE("PadLedController: coalesced, a voice's start is painted by update(), not by its caller",
          "[pad][led]") {
    LockCountingStrip strip;
    crosspad::SyncEventBus bus;
    crosspad::PadLedController leds;
    leds.init(strip, bus, nullptr);
    leds.begin();

    // Immediate mode: the engine's report sends a frame right away.
    int frames = strip.refreshCount;
    leds.notifyNotePlay(3, true);
    REQUIRE(strip.refreshCount == frames + 1);

    // Coalesced (the board): the report, made from the audio render, only
    // records the state -- no pixels, no strip lock, no frame -- and the next
    // tick paints and sends.
    leds.setCoalescedRefresh(true);
    leds.update();
    frames = strip.refreshCount;
    const int locks = strip.locks;
    leds.notifyNotePlay(3, false);
    leds.notifyNotePlay(4, true);
    REQUIRE(strip.locks == locks);
    REQUIRE(strip.refreshCount == frames);
    leds.update();
    REQUIRE(strip.locks > locks);
    REQUIRE(strip.refreshCount == frames + 1);
    leds.update();                      // nothing new: no frame
    REQUIRE(strip.refreshCount == frames + 1);
}

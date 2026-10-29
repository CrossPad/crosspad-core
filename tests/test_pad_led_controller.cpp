// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "test_helpers.hpp"
#include "crosspad/event/SyncEventBus.hpp"
#include "crosspad/pad/PadLedController.hpp"

using namespace crosspad;

// What the strip holds for a pad must be what the controller's state says,
// after every call: a colour changed while the pad is held or sounding is
// painted then, not at the next unrelated repaint (the release).
TEST_CASE("PadLedController: a colour set while the pad is busy reaches the strip", "[pad][led]") {
    const bool coalesced = GENERATE(false, true);
    test::MockLedStrip strip;
    SyncEventBus bus;
    PadLedController leds;
    leds.init(strip, bus, nullptr);
    leds.setCoalescedRefresh(coalesced);
    leds.setFeedbackMode(KeypadFeedbackMode::STANDARD);
    const RgbColor red(255, 0, 0), green(0, 255, 0), blue(0, 0, 255);

    SECTION("setPadColor while the pad's sample sounds") {
        leds.setPadColor(0, red);
        leds.notifyNotePlay(0, true);
        leds.update();
        leds.setPadColor(0, blue);
        leds.update();
        CHECK(strip.pixels[0] == blue);
    }

    SECTION("clearPadColor while the pad is held") {
        leds.setPadPressedColor(1, green);
        leds.notifyNoteOn(1, 127);
        leds.clearPadColor(1);
        leds.update();
        CHECK(strip.pixels[1] == leds.getPressedColor(1));
    }

    SECTION("setPadPlayingColor on a dimmed Loop pad") {
        leds.notifyNoteDimmed(2, true);
        leds.update();
        const RgbColor before = strip.pixels[2];
        leds.setPadPlayingColor(2, green);
        leds.update();
        CHECK_FALSE(strip.pixels[2] == before);
    }
}

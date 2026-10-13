// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"

TEST_CASE("PadAnimator: inactive charging is a refreshed static gauge", "[pad][animator]") {
    test::MockClock clock;
    test::MockLedStrip strip;
    crosspad::PadAnimator animator;
    animator.init(strip, clock);

    animator.startCharging(0.5f);
    animator.setChargingActive(false);
    animator.update();

    const crosspad::RgbColor expected(127, 127, 0);
    for (int i = 0; i < 8; ++i) {
        REQUIRE(strip.pixels[i] == expected);
    }
    for (int i = 8; i < 16; ++i) {
        REQUIRE(strip.pixels[i] == crosspad::RgbColor(0, 0, 0));
    }
    REQUIRE(strip.refreshCount == 1);

    clock.advance(5'000'000);
    animator.update();
    for (int i = 0; i < 8; ++i) {
        REQUIRE(strip.pixels[i] == expected);
    }
    REQUIRE(strip.refreshCount == 2);
}

namespace {
struct AnimatorFixture {
    test::MockClock clock;
    test::MockLedStrip strip;
    crosspad::PadAnimator animator;
    AnimatorFixture() { animator.init(strip, clock); }
};

void fill(crosspad::RgbColor (&frame)[crosspad::PadAnimator::kPadCount], crosspad::RgbColor c) {
    for (auto& px : frame) px = c;
}
} // namespace

TEST_CASE("PadAnimator: a frame's token owns it until another animation takes over", "[pad][animator][owner]") {
    AnimatorFixture f;
    crosspad::RgbColor blue[crosspad::PadAnimator::kPadCount];
    fill(blue, crosspad::RgbColor(0, 0, 255));

    const auto frame = f.animator.showFrame(blue);
    REQUIRE(frame != crosspad::PadAnimator::kNoAnim);
    REQUIRE(f.animator.owns(frame));

    const auto progress = f.animator.startProgress();
    REQUIRE(progress != frame);
    REQUIRE_FALSE(f.animator.owns(frame));
    REQUIRE(f.animator.owns(progress));

    SECTION("the app's repaint shows nothing over the progress bar") {
        crosspad::RgbColor red[crosspad::PadAnimator::kPadCount];
        fill(red, crosspad::RgbColor(255, 0, 0));
        REQUIRE_FALSE(f.animator.updateFrame(frame, red));
        f.animator.update();
        for (const auto& px : f.strip.pixels) REQUIRE(px == crosspad::RgbColor(0, 0, 0));
        REQUIRE(f.animator.owns(progress));
    }

    SECTION("the app's stop leaves the progress bar running") {
        REQUIRE_FALSE(f.animator.stop(frame));
        REQUIRE(f.animator.isActive());
        REQUIRE(f.animator.owns(progress));
    }

    SECTION("the owner's stop ends it") {
        REQUIRE(f.animator.stop(progress));
        REQUIRE_FALSE(f.animator.isActive());
        REQUIRE_FALSE(f.animator.owns(progress));
        REQUIRE_FALSE(f.animator.stop(progress));
    }
}

TEST_CASE("PadAnimator: updateFrame repaints the owner's frame", "[pad][animator][owner]") {
    AnimatorFixture f;
    crosspad::RgbColor blue[crosspad::PadAnimator::kPadCount];
    crosspad::RgbColor green[crosspad::PadAnimator::kPadCount];
    fill(blue, crosspad::RgbColor(0, 0, 255));
    fill(green, crosspad::RgbColor(0, 255, 0));

    const auto token = f.animator.showFrame(blue);
    f.animator.update();
    REQUIRE(f.strip.pixels[0] == crosspad::RgbColor(0, 0, 255));

    REQUIRE(f.animator.updateFrame(token, green));
    f.animator.update();
    for (const auto& px : f.strip.pixels) REQUIRE(px == crosspad::RgbColor(0, 255, 0));
    REQUIRE_FALSE(f.animator.updateFrame(crosspad::PadAnimator::kNoAnim, blue));
}

TEST_CASE("PadAnimator: a finished animation is owned by nobody", "[pad][animator][owner]") {
    AnimatorFixture f;
    const auto token = f.animator.playSuccess(100);
    REQUIRE(f.animator.owns(token));

    f.clock.advance(200'000);
    f.animator.update();
    REQUIRE_FALSE(f.animator.isActive());
    REQUIRE_FALSE(f.animator.owns(token));
    REQUIRE_FALSE(f.animator.stop(token));
}

TEST_CASE("PadAnimator: stopAll ends whatever runs and reports the end once", "[pad][animator][owner]") {
    AnimatorFixture f;
    static int ends;
    ends = 0;
    f.animator.setAnimationStateCallback([](bool active) { if (!active) ++ends; });

    const auto token = f.animator.startCharging(0.5f);
    f.animator.stopAll();
    REQUIRE_FALSE(f.animator.isActive());
    REQUIRE_FALSE(f.animator.owns(token));
    REQUIRE(ends == 1);

    f.animator.stopAll();
    REQUIRE(ends == 1);
}

TEST_CASE("PadAnimator: the completion callback may start the next animation", "[pad][animator][owner]") {
    AnimatorFixture f;
    static crosspad::PadAnimator* animator;
    static crosspad::PadAnimator::AnimToken next;
    static int ends;
    animator = &f.animator;
    next = crosspad::PadAnimator::kNoAnim;
    ends = 0;
    f.animator.setAnimationStateCallback([](bool active) { if (!active) ++ends; });
    f.animator.setCompletionCallback([] { next = animator->playError(); });

    f.animator.playSuccess(100);
    f.clock.advance(200'000);
    f.animator.update();
    REQUIRE(next != crosspad::PadAnimator::kNoAnim);
    REQUIRE(f.animator.owns(next));
    REQUIRE(ends == 0);
}

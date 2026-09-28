#include <catch2/catch_test_macros.hpp>

#include "crosspad/sampletools/SliceMath.hpp"

#include <vector>

using namespace crosspad;

TEST_CASE("slice math: an attack is never shorter than the de-click fade", "[slicemath]") {
    CHECK(SliceMath::attackFrames(0, 44100) == SliceMath::kMinFadeInFrames);
    CHECK(SliceMath::attackFrames(10, 44100) == 441);
    CHECK(SliceMath::attackFrames(60000, 44100) == 44100u * SliceMath::kMaxAttackMs / 1000u);
}

TEST_CASE("slice math: a slice rises over its attack and falls at its end", "[slicemath]") {
    const uint32_t len = 10000, atk = 1000;
    CHECK(SliceMath::sliceGain(0, len, atk) == 0.0f);
    CHECK(SliceMath::sliceGain(500, len, atk) == 0.5f);
    CHECK(SliceMath::sliceGain(atk, len, atk) == 1.0f);
    CHECK(SliceMath::sliceGain(len / 2, len, atk) == 1.0f);
    CHECK(SliceMath::sliceGain(len - 1, len, atk) == 0.0f);
    CHECK(SliceMath::sliceGain(len - 1 - SliceMath::kFadeOutFrames / 2, len, atk) == 0.5f);
    CHECK(SliceMath::sliceGain(len, len, atk) == 0.0f);
    /* An attack longer than the slice: the lower of rise and fall. */
    CHECK(SliceMath::sliceGain(300, 400, 2000) == 0.15f);
    CHECK(SliceMath::sliceGain(390, 400, 2000) == 9.0f / (float)SliceMath::kFadeOutFrames);
}

TEST_CASE("slice math: a semitone ratio and what it does to a length", "[slicemath]") {
    CHECK(SliceMath::pitchRatio(0) == 1.0);
    CHECK(SliceMath::pitchRatio(12) == 2.0);
    CHECK(SliceMath::pitchRatio(-12) == 0.5);
    CHECK(SliceMath::pitchRatio(40) == 2.0);                 // clamped
    CHECK(SliceMath::pitchedFrames(1000, 2.0) == 500);
    CHECK(SliceMath::pitchedFrames(1001, 2.0) == 501);
    CHECK(SliceMath::pitchedFrames(1000, 0.5) == 2000);
    CHECK(SliceMath::pitchedFrames(1000, 1.0) == 1000);
}

TEST_CASE("slice math: a pitched read", "[slicemath]") {
    std::vector<int16_t> ramp(64 * 2);
    for (int i = 0; i < 64; ++i) { ramp[i * 2] = (int16_t)(i * 100); ramp[i * 2 + 1] = (int16_t)(-i * 100); }
    std::vector<int16_t> out(32 * 2);

    /* Unison is the source, frame for frame. */
    SliceMath::readPitched(ramp.data(), 64, 0.0, 1.0, out.data(), 32);
    for (int k = 0; k < 32; ++k) { CHECK(out[k * 2] == k * 100); CHECK(out[k * 2 + 1] == -k * 100); }

    /* An octave up takes every other frame. */
    SliceMath::readPitched(ramp.data(), 64, 0.0, 2.0, out.data(), 30);
    for (int k = 1; k < 30; ++k) CHECK(out[k * 2] == k * 200);

    /* An octave down lands between frames: on a straight line, halfway. */
    SliceMath::readPitched(ramp.data(), 64, 10.0, 0.5, out.data(), 20);
    for (int k = 0; k < 20; ++k) CHECK(out[k * 2] == 1000 + k * 50);

    /* A negative ratio reads backwards, frame for frame at unison. */
    SliceMath::readPitched(ramp.data(), 64, 63.0, -1.0, out.data(), 32);
    for (int k = 0; k < 32; ++k) CHECK(out[k * 2] == (63 - k) * 100);

    /* Past the window's end it holds the last frame, never reads beyond. */
    SliceMath::readPitched(ramp.data(), 64, 62.0, 1.0, out.data(), 4);
    CHECK(out[0] == 6200);
    CHECK(out[2] == 6300);
    CHECK(out[4] == 6300);
    CHECK(out[6] == 6300);
}

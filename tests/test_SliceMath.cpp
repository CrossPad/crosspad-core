// SPDX-License-Identifier: GPL-3.0-or-later
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

TEST_CASE("slice math: a cut snaps to the zero crossing before it", "[slicemath]") {
    /* L+R: +400 +300 +100 -50 -200 -400 -600 -800; a cut at frame 6. */
    const int16_t mono[] = {200, 150, 50, -25, -100, -200, -300, -400};
    std::vector<int16_t> st;
    for (int16_t v : mono) { st.push_back(v); st.push_back(v); }
    CHECK(SliceMath::zeroCrossing(st.data(), 8, 6) == 3);      // -50 nearer zero than +100
    CHECK(SliceMath::zeroCrossing(st.data(), 8, 3) == 3);      // snapped again, it stays
    CHECK(SliceMath::zeroCrossing(st.data(), 8, 2) == 3);      // the pair right after counts

    /* Nothing before it crosses: the first crossing after it. */
    const int16_t up[] = {-300, -200, -100, -10, 40, 90};
    st.clear();
    for (int16_t v : up) { st.push_back(v); st.push_back(v); }
    CHECK(SliceMath::zeroCrossing(st.data(), 6, 1) == 3);
    CHECK(SliceMath::zeroCrossing(st.data(), 6, 3) == 3);

    /* A zero is a crossing; silence never moves; no crossing at all keeps it. */
    std::vector<int16_t> z(16, 0);
    CHECK(SliceMath::zeroCrossing(z.data(), 8, 5) == 5);
    std::vector<int16_t> flat(16, 500);
    CHECK(SliceMath::zeroCrossing(flat.data(), 8, 5) == 5);
}

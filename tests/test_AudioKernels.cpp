// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "crosspad/audio/OutputEq.hpp"
#include "crosspad/dsp/AlignedVector.hpp"
#include "crosspad/dsp/AudioKernels.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using namespace crosspad;
using namespace crosspad::dsp;

namespace {

/* The kernels promise the reference's bits wherever nothing fuses a multiply
 * and an add, which is every host these tests build on. So: compare bits. */
bool sameBits(const float* a, const float* b, size_t n)
{
    return std::memcmp(a, b, n * sizeof(float)) == 0;
}

bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }

/* Noise in about [-1.3, 1.3], so some of it is over full scale, salted with
 * what a render can actually hand a kernel: both zeros, subnormals, exact
 * full scale, and -- when @p nan -- a NaN on either side. */
void fill(float* b, size_t n, uint32_t seed, bool nan = false)
{
    for (size_t i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        b[i] = (static_cast<float>(seed >> 8) / 16777216.0f - 0.5f) * 2.6f;
    }
    const float salt[] = {0.0f, -0.0f, 1e-42f, -1e-42f, 1.0f, -1.0f};
    for (size_t k = 0; k < sizeof(salt) / sizeof(salt[0]) && k * 7 < n; ++k) b[k * 7] = salt[k];
    if (nan && n > 10) {
        b[9] = std::numeric_limits<float>::quiet_NaN();
        b[n - 4] = std::numeric_limits<float>::quiet_NaN();
    }
}

struct ModeGuard {
    KernelMode was = kernelMode();
    ~ModeGuard() { setKernelMode(was); }
};

/* Odd lengths reach the tails; 64 is the board's block. */
const uint32_t kFrames[] = {0, 1, 2, 3, 7, 63, 64, 129};

} // namespace

TEST_CASE("AudioKernels: mac is the reference's sum", "[dsp][kernels]")
{
    for (uint32_t frames : kFrames) {
        const uint32_t n = frames * 2;
        AlignedVector<float> src(n + 1), a(n + 1), b(n + 1);
        fill(src.data(), n, 11 + frames, true);
        fill(a.data(), n, 23 + frames);
        b = a;
        ref::mac(a.data(), src.data(), 0.73f, n);
        fast::mac(b.data(), src.data(), 0.73f, n);
        INFO("frames " << frames);
        CHECK(sameBits(a.data(), b.data(), n));
    }
}

TEST_CASE("AudioKernels: macPanned is the reference's sum", "[dsp][kernels]")
{
    for (uint32_t frames : kFrames) {
        AlignedVector<float> mono(frames + 1), a(frames * 2 + 2), b(frames * 2 + 2), tmp(frames + 1);
        fill(mono.data(), frames, 5 + frames, true);
        fill(a.data(), frames * 2, 6 + frames);
        b = a;
        ref::macPanned(mono.data(), a.data(), frames, 0.3f, -1.7f, tmp.data());
        fast::macPanned(b.data(), mono.data(), frames, 0.3f, -1.7f);
        INFO("frames " << frames);
        CHECK(sameBits(a.data(), b.data(), frames * 2));
    }
}

TEST_CASE("AudioKernels: scaleStereo is the reference's pan", "[dsp][kernels]")
{
    for (uint32_t frames : kFrames) {
        AlignedVector<float> a(frames * 2 + 1), b(frames * 2 + 1);
        fill(a.data(), frames * 2, 31 + frames, true);
        b = a;
        ref::scaleStereo(a.data(), frames, 0.25f, 1.0f);
        fast::scaleStereo(b.data(), frames, 0.25f, 1.0f);
        INFO("frames " << frames);
        CHECK(sameBits(a.data(), b.data(), frames * 2));
    }
}

TEST_CASE("AudioKernels: peaks and the clamp match the reference, NaN and all", "[dsp][kernels]")
{
    ModeGuard guard;
    for (uint32_t frames : kFrames) {
        for (int variant = 0; variant < 3; ++variant) {
            AlignedVector<float> a(frames * 2 + 1), b(frames * 2 + 1);
            fill(a.data(), frames * 2, 41 + frames * 3 + variant, variant == 2);
            /* 0: over full scale; 1: all of it inside, the limiter's case
             * where the clamp pass is skipped; 2: NaN on both sides. */
            if (variant == 1) for (auto& v : a) v *= 0.7f;
            b = a;
            float rl = -1, rr = -1, fl = -1, fr = -1;
            ref::peakStereo(a.data(), frames, rl, rr);
            fast::peakStereo(b.data(), frames, fl, fr);
            INFO("frames " << frames << " variant " << variant);
            CHECK(sameBits(rl, fl));
            CHECK(sameBits(rr, fr));

            setKernelMode(KernelMode::Reference);
            clampPeakStereo(a.data(), frames, rl, rr);
            setKernelMode(KernelMode::Portable);
            clampPeakStereo(b.data(), frames, fl, fr);
            CHECK(sameBits(a.data(), b.data(), frames * 2));
            CHECK(sameBits(rl, fl));
            CHECK(sameBits(rr, fr));
        }
    }
}

TEST_CASE("AudioKernels: biquadStereo is the reference's section, state included", "[dsp][kernels]")
{
    /* A 30 Hz high-pass at 44.1 kHz: the poles nearest the unit circle the EQ
     * makes, where a change in the order of operations would show first. */
    const float w0 = 6.28318530718f * 30.0f / 44100.0f;
    const float cw = std::cos(w0), alpha = std::sin(w0) / (2.0f * 0.7f), a0 = 1.0f + alpha;
    Biquad c;
    c.b0 = (1.0f + cw) * 0.5f / a0;
    c.b1 = -(1.0f + cw) / a0;
    c.b2 = (1.0f + cw) * 0.5f / a0;
    c.a1 = -2.0f * cw / a0;
    c.a2 = (1.0f - alpha) / a0;

    for (uint32_t frames : kFrames) {
        AlignedVector<float> a(frames * 2 + 1), b(frames * 2 + 1);
        float z1a[2] = {0.1f, -0.2f}, z2a[2] = {0.05f, 0.0f};
        float z1b[2] = {0.1f, -0.2f}, z2b[2] = {0.05f, 0.0f};
        /* Several blocks: the state carried from one into the next is the
         * other half of the contract. */
        for (uint32_t block = 0; block < 5; ++block) {
            fill(a.data(), frames * 2, 101 + block * 13 + frames);
            b = a;
            ref::biquadStereo(a.data(), frames, c, z1a, z2a);
            fast::biquadStereo(b.data(), frames, c, z1b, z2b);
            INFO("frames " << frames << " block " << block);
            REQUIRE(sameBits(a.data(), b.data(), frames * 2));
            REQUIRE(sameBits(z1a, z1b, 2));
            REQUIRE(sameBits(z2a, z2b, 2));
        }
    }
}

TEST_CASE("AudioKernels: an EQ chain sounds the same in every kernel mode", "[dsp][kernels][eq]")
{
    ModeGuard guard;
    EqSettings s;
    s.on = true;
    s.band[0] = {EqKind::HighPass, 35.0f, 0.0f, 0.7f};
    s.band[1] = {EqKind::Peak, 180.0f, 4.5f, 1.4f};
    s.band[2] = {EqKind::Peak, 3200.0f, -6.0f, 2.0f};
    s.band[3] = {EqKind::HighShelf, 9000.0f, 3.0f, 0.7f};

    EqChain ref, fast;
    ref.setRate(44100);
    fast.setRate(44100);
    ref.set(s);
    fast.set(s);
    REQUIRE(ref.on());

    constexpr uint32_t kBlock = 64;
    AlignedVector<float> a(kBlock * 2), b(kBlock * 2);
    for (uint32_t block = 0; block < 40; ++block) {
        fill(a.data(), a.size(), 7 + block);
        b = a;
        setKernelMode(KernelMode::Reference);
        ref.process(a.data(), kBlock);
        setKernelMode(KernelMode::Portable);
        fast.process(b.data(), kBlock);
        INFO("block " << block);
        REQUIRE(sameBits(a.data(), b.data(), a.size()));
    }
}

TEST_CASE("AudioKernels: an aligned vector starts on 16 bytes", "[dsp][kernels]")
{
    for (size_t n : {1u, 3u, 64u, 128u, 1000u}) {
        AlignedVector<float> v(n);
        CHECK((reinterpret_cast<uintptr_t>(v.data()) & (kSimdAlign - 1)) == 0);
        v.resize(n * 3);
        CHECK((reinterpret_cast<uintptr_t>(v.data()) & (kSimdAlign - 1)) == 0);
    }
    CHECK(quadAligned(128, AlignedVector<float>(128).data()));
    CHECK_FALSE(quadAligned(126, AlignedVector<float>(128).data()));
}

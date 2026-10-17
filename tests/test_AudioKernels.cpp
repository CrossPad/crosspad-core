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

/* The kernels promise the old loops' bits wherever nothing fuses a multiply
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

/* The render's loops as they were before the kernels: the oracle every kernel
 * is held to, bit for bit. */
namespace oracle {

void mac(float* dst, const float* src, float g, uint32_t n)
{
    for (uint32_t s = 0; s < n; ++s) dst[s] += src[s] * g;
}

void macPanned(const float* mono, float* out, uint32_t frames, float gainL, float gainR)
{
    for (uint32_t i = 0; i < frames; ++i) {
        out[2 * i]     += mono[i] * gainL;
        out[2 * i + 1] += mono[i] * gainR;
    }
}

void scaleStereo(float* s, uint32_t frames, float gl, float gr)
{
    for (uint32_t i = 0; i < frames; ++i) {
        s[i * 2 + 0] *= gl;
        s[i * 2 + 1] *= gr;
    }
}

void peakStereo(const float* buf, uint32_t frames, float& pL, float& pR)
{
    float l0 = 0.0f, r0 = 0.0f;
    for (uint32_t i = 0; i < frames; ++i) {
        float l = buf[i * 2 + 0]; if (l < 0) l = -l;
        float r = buf[i * 2 + 1]; if (r < 0) r = -r;
        if (l > l0) l0 = l;
        if (r > r0) r0 = r;
    }
    pL = l0;
    pR = r0;
}

/* Every block clamped, over full scale or not. */
void clampPeakStereo(float* dst, uint32_t frames, float& pL, float& pR)
{
    float pl = 0.0f, pr = 0.0f;
    for (uint32_t i = 0; i < frames; ++i) {
        float l = dst[i * 2 + 0];
        float r = dst[i * 2 + 1];
        if (l >  1.0f) l =  1.0f;
        if (l < -1.0f) l = -1.0f;
        if (r >  1.0f) r =  1.0f;
        if (r < -1.0f) r = -1.0f;
        dst[i * 2 + 0] = l;
        dst[i * 2 + 1] = r;
        float aL = l < 0 ? -l : l;
        float aR = r < 0 ? -r : r;
        if (aL > pl) pl = aL;
        if (aR > pr) pr = aR;
    }
    pL = pl;
    pR = pr;
}

/* One section, a channel at a time, as EqChain ran it. */
void biquadStereo(float* io, uint32_t frames, const Biquad& s, float* z1s, float* z2s)
{
    for (int ch = 0; ch < 2; ++ch) {
        float z1 = z1s[ch], z2 = z2s[ch];
        for (uint32_t f = 0; f < frames; ++f) {
            const float x = io[f * 2 + ch];
            const float y = s.b0 * x + z1;
            z1 = s.b1 * x - s.a1 * y + z2;
            z2 = s.b2 * x - s.a2 * y;
            io[f * 2 + ch] = y;
        }
        z1s[ch] = z1;
        z2s[ch] = z2;
    }
}

} // namespace oracle

/* FNV-1a over the bits of every sample: what a recorded run is compared by. */
uint64_t digest(const std::vector<float>& v)
{
    uint64_t h = 1469598103934665603ull;
    for (float f : v) {
        uint32_t b;
        std::memcpy(&b, &f, sizeof b);
        for (int i = 0; i < 4; ++i) {
            h ^= (b >> (i * 8)) & 0xFFu;
            h *= 1099511628211ull;
        }
    }
    return h;
}

/* Odd lengths reach the tails; 64 is the board's block. */
const uint32_t kFrames[] = {0, 1, 2, 3, 7, 63, 64, 129};

} // namespace

TEST_CASE("AudioKernels: mac is the old loop's sum", "[dsp][kernels]")
{
    for (uint32_t frames : kFrames) {
        const uint32_t n = frames * 2;
        AlignedVector<float> src(n + 1), a(n + 1), b(n + 1);
        fill(src.data(), n, 11 + frames, true);
        fill(a.data(), n, 23 + frames);
        b = a;
        oracle::mac(a.data(), src.data(), 0.73f, n);
        mac(b.data(), src.data(), 0.73f, n);
        INFO("frames " << frames);
        CHECK(sameBits(a.data(), b.data(), n));
    }
}

TEST_CASE("AudioKernels: macPanned is the old loop's sum", "[dsp][kernels]")
{
    for (uint32_t frames : kFrames) {
        AlignedVector<float> mono(frames + 1), a(frames * 2 + 2), b(frames * 2 + 2);
        fill(mono.data(), frames, 5 + frames, true);
        fill(a.data(), frames * 2, 6 + frames);
        b = a;
        oracle::macPanned(mono.data(), a.data(), frames, 0.3f, -1.7f);
        macPanned(b.data(), mono.data(), frames, 0.3f, -1.7f);
        INFO("frames " << frames);
        CHECK(sameBits(a.data(), b.data(), frames * 2));
    }
}

TEST_CASE("AudioKernels: scaleStereo is the old loop's pan", "[dsp][kernels]")
{
    for (uint32_t frames : kFrames) {
        AlignedVector<float> a(frames * 2 + 1), b(frames * 2 + 1);
        fill(a.data(), frames * 2, 31 + frames, true);
        b = a;
        oracle::scaleStereo(a.data(), frames, 0.25f, 1.0f);
        scaleStereo(b.data(), frames, 0.25f, 1.0f);
        INFO("frames " << frames);
        CHECK(sameBits(a.data(), b.data(), frames * 2));
    }
}

TEST_CASE("AudioKernels: peaks and the clamp match the old loops, NaN and all", "[dsp][kernels]")
{
    for (uint32_t frames : kFrames) {
        for (int variant = 0; variant < 3; ++variant) {
            AlignedVector<float> a(frames * 2 + 1), b(frames * 2 + 1);
            fill(a.data(), frames * 2, 41 + frames * 3 + variant, variant == 2);
            /* 0: over full scale; 1: all of it inside, the limiter's case
             * where the clamp pass is skipped; 2: NaN on both sides. */
            if (variant == 1) for (auto& v : a) v *= 0.7f;
            b = a;
            float rl = -1, rr = -1, fl = -1, fr = -1;
            oracle::peakStereo(a.data(), frames, rl, rr);
            peakStereo(b.data(), frames, fl, fr);
            INFO("frames " << frames << " variant " << variant);
            CHECK(sameBits(rl, fl));
            CHECK(sameBits(rr, fr));

            oracle::clampPeakStereo(a.data(), frames, rl, rr);
            clampPeakStereo(b.data(), frames, fl, fr);
            CHECK(sameBits(a.data(), b.data(), frames * 2));
            CHECK(sameBits(rl, fl));
            CHECK(sameBits(rr, fr));
        }
    }
}

TEST_CASE("AudioKernels: biquadStereo is the old section, state included", "[dsp][kernels]")
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
            oracle::biquadStereo(a.data(), frames, c, z1a, z2a);
            biquadStereo(b.data(), frames, c, z1b, z2b);
            INFO("frames " << frames << " block " << block);
            REQUIRE(sameBits(a.data(), b.data(), frames * 2));
            REQUIRE(sameBits(z1a, z1b, 2));
            REQUIRE(sameBits(z2a, z2b, 2));
        }
    }
}

TEST_CASE("AudioKernels: an EQ chain sounds as it did through the loops before the kernels",
          "[dsp][kernels][eq]")
{
    /* EqChain's sections are its own, so the chain is held to a recording:
     * the digest of these 40 blocks through the section-at-a-time loops
     * (oracle::biquadStereo), taken before those loops left the library. */
    EqSettings s;
    s.on = true;
    s.band[0] = {EqKind::HighPass, 35.0f, 0.0f, 0.7f};
    s.band[1] = {EqKind::Peak, 180.0f, 4.5f, 1.4f};
    s.band[2] = {EqKind::Peak, 3200.0f, -6.0f, 2.0f};
    s.band[3] = {EqKind::HighShelf, 9000.0f, 3.0f, 0.7f};

    EqChain chain;
    chain.setRate(44100);
    chain.set(s);
    REQUIRE(chain.on());

    constexpr uint32_t kBlock = 64;
    AlignedVector<float> a(kBlock * 2);
    std::vector<float> all;
    for (uint32_t block = 0; block < 40; ++block) {
        fill(a.data(), a.size(), 7 + block);
        chain.process(a.data(), kBlock);
        all.insert(all.end(), a.begin(), a.end());
    }
    REQUIRE(all.size() == 5120);
    CHECK(digest(all) == 0xba2c69f4bd1e9bf5ull);
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

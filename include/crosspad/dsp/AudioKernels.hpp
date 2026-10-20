// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file AudioKernels.hpp
 * @brief The render's per-sample loops over float32 audio, written for the
 *        ESP32-S3 and portable everywhere else.
 *
 * What "SIMD" means on this core. The S3's PIE has 128-bit vector lanes of
 * 8, 16 and 32-bit *integers* only; the FPU is scalar single precision. What
 * PIE does give float code is EE.LDF.128 / EE.STF.128: four floats between
 * memory and four FPU registers in one instruction, post-incremented. A loop
 * that is mostly loads and stores -- a send, a gain -- spends three of every
 * four of its instructions on memory, so that is where it wins. Measured on
 * the board, a peak and a biquad did not gain from it, and have no PIE path.
 * The arithmetic stays on the FPU, which has a fused madd.s.
 *
 * The portable versions are what the PIE ones fall back to (a buffer not on
 * 16 bytes, a length not a multiple of four) and what every other platform
 * runs. They are written so GCC on Xtensa emits branchless code -- abs.s and
 * movt.s instead of a compare-and-branch per sample, which is what the
 * render's `x < 0 ? -x : x` and `if (x > peak)` turned into -- and so that
 * each loop body holds independent chains for the FPU's latency to hide in.
 *
 * Bit-exact against the loops they replaced on every platform without a
 * fused multiply-add, which is every host the tests run on (the tests keep
 * those loops as their oracle). Where the device fuses (mac*: the esp-dsp
 * route rounded the product and the sum separately, madd.s rounds once) the
 * difference is at most half an ulp of each sum.
 *
 * Which kernel runs: the portable one here, unless the platform registered
 * its own with setAudioKernels() -- the ESP-IDF platform registers the PIE
 * loops for the S3, which fall back to these for a buffer not on 16 bytes.
 * No vendor or ISA code is in core, so core and its consumers never disagree
 * on what an inline function holds. The board measured the old loops, the
 * portable kernels and PIE against each other in the real render (16.10.2026)
 * and PIE won where it exists.
 */

#include "crosspad/audio/AudioFormatConvert.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>

namespace crosspad::dsp {

/// One biquad section, normalised by a0.
struct Biquad { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

/// Whether every pointer is on 16 bytes and @p n a multiple of four: what
/// EE.LDF.128 / EE.STF.128 need. They ignore the low address bits, so a
/// buffer off by four bytes would be read from the wrong place, not trapped.
inline bool quadAligned(uint32_t n, const void* a, const void* b = nullptr)
{
    return ((reinterpret_cast<uintptr_t>(a) | reinterpret_cast<uintptr_t>(b) | n * 4u) & 15u) == 0;
}

/// Larger of two, as the reference loops pick it: @p cur unless @p v is
/// greater -- a NaN never wins. One olt.s and one movt.s on Xtensa.
inline float maxOf(float cur, float v) { return v > cur ? v : cur; }

namespace fast {

inline void mac(float* __restrict dst, const float* __restrict src, float g, uint32_t n)
{
    uint32_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const float s0 = src[i], s1 = src[i + 1], s2 = src[i + 2], s3 = src[i + 3];
        dst[i]     += s0 * g;
        dst[i + 1] += s1 * g;
        dst[i + 2] += s2 * g;
        dst[i + 3] += s3 * g;
    }
    for (; i < n; ++i) dst[i] += src[i] * g;
}

inline void macPanned(float* __restrict out, const float* __restrict mono, uint32_t frames,
                      float gl, float gr)
{
    for (uint32_t i = 0; i < frames; ++i) {
        const float m = mono[i];
        out[i * 2 + 0] += m * gl;
        out[i * 2 + 1] += m * gr;
    }
}

inline void scaleStereo(float* buf, uint32_t frames, float gl, float gr)
{
    for (uint32_t i = 0; i < frames; ++i) {
        buf[i * 2 + 0] *= gl;
        buf[i * 2 + 1] *= gr;
    }
}

inline void peakStereo(const float* buf, uint32_t frames, float& pL, float& pR)
{
    float l = 0.0f, r = 0.0f;
    for (uint32_t i = 0; i < frames; ++i) {
        l = maxOf(l, std::fabs(buf[i * 2 + 0]));
        r = maxOf(r, std::fabs(buf[i * 2 + 1]));
    }
    pL = l;
    pR = r;
}

/* The clamp itself, for a block that went over full scale. Comparisons, so a
 * NaN passes through and is left out of the peaks. */
inline void clampPeakStereo(float* buf, uint32_t frames, float& pL, float& pR)
{
    float pl = 0.0f, pr = 0.0f;
    for (uint32_t i = 0; i < frames; ++i) {
        float l = buf[i * 2 + 0];
        float r = buf[i * 2 + 1];
        if (l >  1.0f) l =  1.0f;
        if (l < -1.0f) l = -1.0f;
        if (r >  1.0f) r =  1.0f;
        if (r < -1.0f) r = -1.0f;
        buf[i * 2 + 0] = l;
        buf[i * 2 + 1] = r;
        pl = maxOf(pl, std::fabs(l));
        pr = maxOf(pr, std::fabs(r));
    }
    pL = pl;
    pR = pr;
}

/* Both channels in one pass: two recursions the FPU can overlap, where one
 * channel at a time leaves it waiting on each sample's madd before the next.
 * Two frames a turn, so the state moves between registers by renaming
 * rather than by a mov.s each. The expressions are the reference's, term
 * for term, so the rounding is. */
inline void biquadStereo(float* io, uint32_t frames, const Biquad& c, float* z1, float* z2)
{
    const float b0 = c.b0, b1 = c.b1, b2 = c.b2, a1 = c.a1, a2 = c.a2;
    float z1l = z1[0], z1r = z1[1], z2l = z2[0], z2r = z2[1];
    uint32_t f = 0;
    for (; f + 2 <= frames; f += 2) {
        float* p = io + f * 2;
        const float xl0 = p[0], xr0 = p[1], xl1 = p[2], xr1 = p[3];
        const float yl0 = b0 * xl0 + z1l;
        const float yr0 = b0 * xr0 + z1r;
        z1l = b1 * xl0 - a1 * yl0 + z2l;
        z1r = b1 * xr0 - a1 * yr0 + z2r;
        z2l = b2 * xl0 - a2 * yl0;
        z2r = b2 * xr0 - a2 * yr0;
        const float yl1 = b0 * xl1 + z1l;
        const float yr1 = b0 * xr1 + z1r;
        z1l = b1 * xl1 - a1 * yl1 + z2l;
        z1r = b1 * xr1 - a1 * yr1 + z2r;
        z2l = b2 * xl1 - a2 * yl1;
        z2r = b2 * xr1 - a2 * yr1;
        p[0] = yl0; p[1] = yr0; p[2] = yl1; p[3] = yr1;
    }
    if (f < frames) {
        float* p = io + f * 2;
        const float xl = p[0], xr = p[1];
        const float yl = b0 * xl + z1l;
        const float yr = b0 * xr + z1r;
        z1l = b1 * xl - a1 * yl + z2l;
        z1r = b1 * xr - a1 * yr + z2r;
        z2l = b2 * xl - a2 * yl;
        z2r = b2 * xr - a2 * yr;
        p[0] = yl; p[1] = yr;
    }
    z1[0] = z1l; z1[1] = z1r; z2[0] = z2l; z2[1] = z2r;
}

/// float to int32 as a saturating truncation does it: toward zero, the ends
/// of the range past it, NaN to INT32_MAX. One TRUNC.S on the S3.
inline int truncSat(float y)
{
    if (y != y || y >= 2147483648.0f) return INT32_MAX;
    if (y <= -2147483648.0f) return INT32_MIN;
    return static_cast<int>(y);
}

/// To the int16 range. One CLAMPS on the S3.
inline int clampS16(int t) { return std::min(std::max(t, -32768), 32767); }

/**
 * @brief floatToInt16() with the two per-sample steps a platform may do in one
 *        instruction each: @p truncSat and @p clampS16 behave as the portable
 *        functions of those names. The kernel and the platform's S3 version
 *        are this one loop.
 *
 * A sample is a multiply, the truncation, the clamp, a max for the -32767 the
 * reference stops at (the clamp goes to -32768), and the store. A NaN has no
 * cheap per-sample answer -- a saturating truncation makes it INT32_MAX, as it
 * does an overflow -- so the block's sum asks once: a NaN in makes it NaN, and
 * that block, which the render never produces, goes through the reference.
 * Eight a turn, two sums, is what the FPU's latency wants.
 */
template <typename TruncSat, typename ClampS16>
inline void floatToInt16With(const float* src, int16_t* dst, uint32_t n,
                             TruncSat truncSat, ClampS16 clampS16)
{
    constexpr uint32_t kLanes = 8;
    const float* s = src;
    int16_t* d = dst;
    float sumEven = 0.0f, sumOdd = 0.0f;
    for (uint32_t k = n / kLanes; k; --k, s += kLanes, d += kLanes) {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC unroll 8
#endif
        for (uint32_t j = 0; j < kLanes; ++j) {
            const float x = s[j];
            (j & 1 ? sumOdd : sumEven) += x;
            d[j] = static_cast<int16_t>(std::max(clampS16(truncSat(x * kFloatScaleToInt16)),
                                                 -static_cast<int>(kFloatScaleToInt16)));
        }
    }
    const float sum = sumEven + sumOdd;
    const uint32_t body = n - n % kLanes;
    if (sum != sum) crosspad::floatToInt16(src, dst, body);
    crosspad::floatToInt16(src + body, dst + body, n - body);
}

} // namespace fast

// ── Dispatch ─────────────────────────────────────────────────────────────────
//
// Out of line (src/dsp/AudioKernels.cpp): one copy of each kernel in the
// image, not three inlined into every caller -- the image is PSRAM too. A
// call is one relaxed load and an indirect call, a few cycles against a few
// hundred for the block.

/// dst[i] += src[i] * g over @p n samples. @p dst and @p src must not overlap.
void mac(float* dst, const float* src, float g, uint32_t n);

/// out[L] += mono * gl, out[R] += mono * gr: a mono voice into a stereo bus.
void macPanned(float* out, const float* mono, uint32_t frames, float gl, float gr);

/// Left samples of interleaved stereo times @p gl, right times @p gr, in place.
void scaleStereo(float* buf, uint32_t frames, float gl, float gr);

/// The largest |sample| on each side of interleaved stereo; NaN is ignored.
void peakStereo(const float* buf, uint32_t frames, float& pL, float& pR);

/// Clamp interleaved stereo to [-1, 1] in place (NaN passes through) and
/// return each side's peak after it.
///
/// The peak first: a block inside [-1, 1] -- every block once the limiter is
/// in front of this -- has nothing to clamp, and its peak after the clamp is
/// the one before. Only a block over full scale takes the clamping pass. The
/// same results as clamping every block: the largest of min(|x|, 1) is
/// min(largest |x|, 1), and NaN is left out of both.
void clampPeakStereo(float* buf, uint32_t frames, float& pL, float& pR);

/// One transposed direct-form II section over interleaved stereo, in place.
/// @p z1 and @p z2 are the two channels' state, [left, right].
void biquadStereo(float* io, uint32_t frames, const Biquad& c, float* z1, float* z2);

/// A bus to int16 for the I2S and USB writes, the same bits as
/// crosspad::floatToInt16: clamp to [-1, 1], times 32767, truncate; NaN is 0.
/// @p n counts samples, not frames.
void floatToInt16(const float* src, int16_t* dst, uint32_t n);

/**
 * @brief A platform's versions of the dispatched kernels. Each must give the
 *        results the portable one documents; a null entry keeps the portable
 *        one. clampPeakStereo() is peakStereo() plus the portable clamp.
 */
struct AudioKernelTable {
    void (*mac)(float* dst, const float* src, float g, uint32_t n) = nullptr;
    void (*macPanned)(float* out, const float* mono, uint32_t frames, float gl, float gr) = nullptr;
    void (*scaleStereo)(float* buf, uint32_t frames, float gl, float gr) = nullptr;
    void (*peakStereo)(const float* buf, uint32_t frames, float& pL, float& pR) = nullptr;
    void (*biquadStereo)(float* io, uint32_t frames, const Biquad& c, float* z1, float* z2) = nullptr;
    void (*floatToInt16)(const float* src, int16_t* dst, uint32_t n) = nullptr;
};

/// Register the platform's kernels, before audio starts; the table is copied.
/// nullptr puts every portable kernel back.
void setAudioKernels(const AudioKernelTable* table);

} // namespace crosspad::dsp

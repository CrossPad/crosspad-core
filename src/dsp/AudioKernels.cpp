// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/dsp/AudioKernels.hpp"

#include "crosspad/audio/AudioFormatConvert.hpp"

#include <algorithm>

namespace crosspad::dsp {

namespace {

/* The two steps the S3 does in one instruction each, which GCC would spend a
 * compare and a branch a sample on: TRUNC.S, which saturates and turns a NaN
 * into INT_MAX, and CLAMPS, the clamp to int16. The other platforms get the
 * same two as plain code, so the loop around them is the one the host tests
 * run. */
#if defined(__XTENSA__) && !defined(__XTENSA_SOFT_FLOAT__)
inline int truncSat(float y)
{
    int t;
    asm("trunc.s %0, %1, 0" : "=r"(t) : "f"(y));
    return t;
}

inline int clampS16(int t)
{
    int r;
    asm("clamps %0, %1, 15" : "=r"(r) : "r"(t));
    return r;
}
#else
inline int truncSat(float y)
{
    if (y != y || y >= 2147483648.0f) return INT32_MAX;
    if (y <= -2147483648.0f) return INT32_MIN;
    return static_cast<int>(y);
}

inline int clampS16(int t) { return std::min(std::max(t, -32768), 32767); }
#endif

}  // namespace

void mac(float* dst, const float* src, float g, uint32_t n)
{
#if CROSSPAD_DSP_PIE
    if (quadAligned(n, dst, src)) { pie::mac(dst, src, g, n); return; }
#endif
    fast::mac(dst, src, g, n);
}

void macPanned(float* out, const float* mono, uint32_t frames, float gl, float gr)
{
#if CROSSPAD_DSP_PIE
    if (quadAligned(frames, out, mono)) { pie::macPanned(out, mono, frames, gl, gr); return; }
#endif
    fast::macPanned(out, mono, frames, gl, gr);
}

void scaleStereo(float* buf, uint32_t frames, float gl, float gr)
{
#if CROSSPAD_DSP_PIE
    if (quadAligned(frames * 2, buf)) { pie::scaleStereo(buf, frames, gl, gr); return; }
#endif
    fast::scaleStereo(buf, frames, gl, gr);
}

void peakStereo(const float* buf, uint32_t frames, float& pL, float& pR)
{
    /* No PIE path: on the board (16.10.2026) its four-float loads read a
     * channel's block no faster than the portable loop (565 vs 595 cycles
     * alone) and the render's peaks took 39 us against 32 with them. */
    fast::peakStereo(buf, frames, pL, pR);
}

void clampPeakStereo(float* buf, uint32_t frames, float& pL, float& pR)
{
    peakStereo(buf, frames, pL, pR);
    if (pL > 1.0f || pR > 1.0f) fast::clampPeakStereo(buf, frames, pL, pR);
}

void biquadStereo(float* io, uint32_t frames, const Biquad& c, float* z1, float* z2)
{
    /* No PIE path: the section is FPU-bound and the wide loads saved nothing
     * on the board (1182 vs 1151 cycles a block, the four-float version
     * spilling a register); with it and the PIE peaks the output stage took
     * 120 us a block against 110. */
    fast::biquadStereo(io, frames, c, z1, z2);
}

void floatToInt16(const float* src, int16_t* dst, uint32_t n)
{
    /* A sample is a multiply, TRUNC.S, CLAMPS, a max for the -32767 the
     * reference stops at (CLAMPS goes to -32768), and the store. A NaN has no
     * cheap per-sample answer -- TRUNC.S makes it INT_MAX, as it does an
     * overflow -- so the block's sum asks once: a NaN in makes it NaN, and
     * that block, which the render never produces, goes through the
     * reference. Eight a turn, two sums, is what the FPU's latency wants. */
    const float* s = src;
    int16_t* d = dst;
    float sumEven = 0.0f, sumOdd = 0.0f;
    for (uint32_t k = n / 8; k; --k, s += 8, d += 8) {
#pragma GCC unroll 8
        for (int j = 0; j < 8; ++j) {
            const float x = s[j];
            (j & 1 ? sumOdd : sumEven) += x;
            d[j] = static_cast<int16_t>(std::max(clampS16(truncSat(x * 32767.0f)), -32767));
        }
    }
    const float sum = sumEven + sumOdd;
    const uint32_t body = n & ~7u;
    if (sum != sum) crosspad::floatToInt16(src, dst, body);
    crosspad::floatToInt16(src + body, dst + body, n - body);
}

} // namespace crosspad::dsp

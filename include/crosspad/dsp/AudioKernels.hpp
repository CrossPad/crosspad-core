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
 * Which kernel runs: PIE where it is compiled in and the buffers are on 16
 * bytes, the portable one otherwise. The board measured the old loops, the
 * portable kernels and PIE against each other in the real render (16.10.2026)
 * and PIE won where it exists; that switch is gone.
 */

#include <cmath>
#include <cstdint>

#if defined(CROSSPAD_SIMD_PIE) && defined(__XTENSA__)
#define CROSSPAD_DSP_PIE 1
#else
#define CROSSPAD_DSP_PIE 0
#endif

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

#if CROSSPAD_DSP_PIE
namespace pie {
/* Four floats, lowest address in the last operand. The memory clobber keeps
 * GCC from moving a plain load or store of the same buffer across them. */
#define CP_LDF4(p, x0, x1, x2, x3) \
    asm volatile("ee.ldf.128.ip %0, %1, %2, %3, %4, 16" \
                 : "=f"(x3), "=f"(x2), "=f"(x1), "=f"(x0), "+r"(p) :: "memory")
#define CP_STF4(p, x0, x1, x2, x3) \
    asm volatile("ee.stf.128.ip %1, %2, %3, %4, %0, 16" \
                 : "+r"(p) : "f"(x3), "f"(x2), "f"(x1), "f"(x0) : "memory")

inline void mac(float* dst, const float* src, float g, uint32_t n)
{
    const float* d = dst;
    for (uint32_t i = 0; i < n; i += 4) {
        float s0, s1, s2, s3, d0, d1, d2, d3;
        CP_LDF4(src, s0, s1, s2, s3);
        CP_LDF4(d, d0, d1, d2, d3);
        d0 += s0 * g; d1 += s1 * g; d2 += s2 * g; d3 += s3 * g;
        CP_STF4(dst, d0, d1, d2, d3);
    }
}

/* Four mono frames against eight interleaved ones a turn. */
inline void macPanned(float* out, const float* mono, uint32_t frames, float gl, float gr)
{
    const float* o = out;
    for (uint32_t i = 0; i < frames; i += 4) {
        float m0, m1, m2, m3, l0, r0, l1, r1, l2, r2, l3, r3;
        CP_LDF4(mono, m0, m1, m2, m3);
        CP_LDF4(o, l0, r0, l1, r1);
        CP_LDF4(o, l2, r2, l3, r3);
        l0 += m0 * gl; r0 += m0 * gr; l1 += m1 * gl; r1 += m1 * gr;
        l2 += m2 * gl; r2 += m2 * gr; l3 += m3 * gl; r3 += m3 * gr;
        CP_STF4(out, l0, r0, l1, r1);
        CP_STF4(out, l2, r2, l3, r3);
    }
}

inline void scaleStereo(float* buf, uint32_t frames, float gl, float gr)
{
    const float* s = buf;
    for (uint32_t i = 0; i < frames; i += 2) {
        float l0, r0, l1, r1;
        CP_LDF4(s, l0, r0, l1, r1);
        l0 *= gl; r0 *= gr; l1 *= gl; r1 *= gr;
        CP_STF4(buf, l0, r0, l1, r1);
    }
}

#undef CP_LDF4
#undef CP_STF4
} // namespace pie
#endif // CROSSPAD_DSP_PIE

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

} // namespace fast

// ── Dispatch ─────────────────────────────────────────────────────────────────
//
// Out of line (src/dsp/AudioKernels.cpp, built -O2 on the board): one copy of
// each kernel in the image, not three inlined into every caller -- the image
// is PSRAM too. A call is a few cycles against a few hundred for the block.

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

} // namespace crosspad::dsp

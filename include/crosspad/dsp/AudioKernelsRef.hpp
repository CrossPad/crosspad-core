// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file AudioKernelsRef.hpp
 * @brief The render's loops as they were before AudioKernels.hpp: the oracle
 *        the host tests hold the kernels to, and the "before" SIMD_BENCH and
 *        SIMD_MODE 0 time on the board. Not for new code.
 */

#include <cstdint>

#if defined(CROSSPAD_USE_ESP_DSP) && __has_include(<esp_dsp.h>)
#include <esp_dsp.h>
#define CROSSPAD_DSP_REF_ESP_DSP 1
#else
#define CROSSPAD_DSP_REF_ESP_DSP 0
#endif

namespace crosspad::dsp {

/// One biquad section, normalised by a0.
struct Biquad { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

namespace ref {

/* The mixer's send: esp-dsp's two passes on the board (it has no fused
 * multiply-add for float), a plain loop elsewhere. The product goes through
 * a block on the stack instead of the mixer's scratch; the arithmetic is the
 * same. */
inline void mac(float* dst, const float* src, float g, uint32_t n)
{
#if CROSSPAD_DSP_REF_ESP_DSP
    constexpr uint32_t kChunk = 64;
    float tmp[kChunk];
    for (uint32_t i = 0; i < n; i += kChunk) {
        const int k = static_cast<int>(n - i < kChunk ? n - i : kChunk);
        dsps_mulc_f32(src + i, tmp, k, g, 1, 1);
        dsps_add_f32(dst + i, tmp, dst + i, k, 1, 1, 1);
    }
#else
    for (uint32_t s = 0; s < n; ++s) dst[s] += src[s] * g;
#endif
}

/* PitchedInstrument's accumulate: four esp-dsp passes on the board, through
 * @p tmp (frames floats). */
inline void macPanned(const float* mono, float* out, uint32_t frames, float gainL, float gainR,
                      float* tmp)
{
#if CROSSPAD_DSP_REF_ESP_DSP
    const int n = static_cast<int>(frames);
    dsps_mulc_f32(mono, tmp, n, gainL, 1, 1);
    dsps_add_f32(out, tmp, out, n, 2, 1, 2);
    dsps_mulc_f32(mono, tmp, n, gainR, 1, 1);
    dsps_add_f32(out + 1, tmp, out + 1, n, 2, 1, 2);
#else
    (void)tmp;
    for (uint32_t i = 0; i < frames; ++i) {
        out[2 * i]     += mono[i] * gainL;
        out[2 * i + 1] += mono[i] * gainR;
    }
#endif
}

inline void scaleStereo(float* s, uint32_t frames, float gl, float gr)
{
    for (uint32_t i = 0; i < frames; ++i) {
        s[i * 2 + 0] *= gl;
        s[i * 2 + 1] *= gr;
    }
}

inline void peakStereo(const float* buf, uint32_t frames, float& pL, float& pR)
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

inline void clampPeakStereo(float* dst, uint32_t frames, float& pL, float& pR)
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

/// One section, a channel at a time, as EqChain ran it.
inline void biquadStereo(float* io, uint32_t frames, const Biquad& s, float* z1s, float* z2s)
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

} // namespace ref
} // namespace crosspad::dsp

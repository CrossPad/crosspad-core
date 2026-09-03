// SPDX-License-Identifier: MIT
#include "crosspad/dsp/FftResampler.hpp"

#include <cmath>
#include <cstring>

namespace crosspad {

namespace {

constexpr uint32_t kMaxBlock = 4096;       // largest analysis block, N
constexpr uint32_t kOversample = 2;        // spectral zero-pad factor

/// Analysis block for @p fft: the inverse runs at 2N, so N is capped by half
/// the largest transform the backend accepts. 0 means the backend is too
/// small to resample with at all.
uint32_t blockFor(const IFft& fft) {
    uint32_t n = fft.maxSize() / kOversample;
    if (n > kMaxBlock) n = kMaxBlock;
    if (n < kMinFftSize) return 0;
    return n;
}

int16_t toInt16(float v) {
    const float r = (v >= 0.0f) ? (v + 0.5f) : (v - 0.5f);
    if (r >  32767.0f) return  32767;
    if (r < -32768.0f) return -32768;
    return static_cast<int16_t>(r);
}

/// Cubic Hermite (Catmull-Rom) at y[i] + frac, taps outside [0, n) read as 0.
float hermite(const float* y, uint32_t n, int64_t i, float f) {
    const float y0 = (i - 1 >= 0 && i - 1 < (int64_t)n) ? y[i - 1] : 0.0f;
    const float y1 = (i     >= 0 && i     < (int64_t)n) ? y[i]     : 0.0f;
    const float y2 = (i + 1 >= 0 && i + 1 < (int64_t)n) ? y[i + 1] : 0.0f;
    const float y3 = (i + 2 >= 0 && i + 2 < (int64_t)n) ? y[i + 2] : 0.0f;
    const float c0 = y1;
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + c0;
}

int64_t ceilDouble(double v) {
    return static_cast<int64_t>(std::ceil(v));
}

} // namespace

uint32_t resampledLength(uint32_t inCount, uint32_t inRate, uint32_t outRate) {
    if (inCount == 0 || inRate == 0 || outRate == 0) return 0;
    const double n = static_cast<double>(inCount) * outRate / inRate;
    return static_cast<uint32_t>(n + 0.5);
}

bool fftResample(IFft& fft, const int16_t* in, uint32_t inCount, uint32_t inRate,
                 int16_t* out, uint32_t outCount, uint32_t outRate,
                 void* (*scratchAlloc)(size_t), void (*scratchFree)(void*)) {
    if (!in || !out || !scratchAlloc || !scratchFree) return false;
    if (inCount == 0 || inRate == 0 || outRate == 0 || outCount == 0) return false;

    const uint32_t block = blockFor(fft);
    if (block == 0) return false;
    const uint32_t hop = block / 2;              // 50 % overlap
    const uint32_t padded = block * kOversample;

    const double ratio = static_cast<double>(outRate) / static_cast<double>(inRate);

    // Accumulator spans one block's worth of output plus interpolation slack.
    const uint32_t accLen = static_cast<uint32_t>(block * ratio) + 8;
    const size_t floats = static_cast<size_t>(block) * 2   // forward FFT buffer
                        + static_cast<size_t>(padded) * 2     // zero-padded spectrum / block
                        + block                            // Hann window
                        + accLen;                           // output accumulator
    float* mem = static_cast<float*>(scratchAlloc(floats * sizeof(float)));
    if (!mem) return false;

    float* fbuf = mem;
    float* ybuf = fbuf + static_cast<size_t>(block) * 2;
    float* win  = ybuf + static_cast<size_t>(padded) * 2;
    float* acc  = win + block;

    for (uint32_t i = 0; i < block; ++i) {
        win[i] = 0.5f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI)
                                      * static_cast<float>(i) / static_cast<float>(block));
    }
    std::memset(acc, 0, accLen * sizeof(float));
    std::memset(out, 0, static_cast<size_t>(outCount) * sizeof(int16_t));

    // Anti-alias cutoff, in bins of the block-point spectrum.
    const uint32_t nyq = (outRate < inRate) ? outRate : inRate;
    uint32_t cutoff = static_cast<uint32_t>(
        static_cast<double>(block) * nyq / (2.0 * inRate));
    if (cutoff > block / 2) cutoff = block / 2;

    int64_t accBase = 0;
    bool ok = true;

    for (int64_t b = -1; b * static_cast<int64_t>(hop) < static_cast<int64_t>(inCount); ++b) {
        const int64_t p0 = b * static_cast<int64_t>(hop);

        int64_t n0 = ceilDouble(static_cast<double>(p0) * ratio);
        int64_t n1 = ceilDouble(static_cast<double>(p0 + block) * ratio);
        if (n0 < 0) n0 = 0;
        if (n1 > static_cast<int64_t>(outCount)) n1 = static_cast<int64_t>(outCount);
        if (n1 <= n0) continue;

        // ── flush finished output, slide the accumulator to n0 ──
        if (n0 > accBase) {
            const int64_t shift = n0 - accBase;
            for (int64_t i = 0; i < shift; ++i) {
                const int64_t idx = accBase + i;
                if (idx >= static_cast<int64_t>(outCount)) break;
                if (i < static_cast<int64_t>(accLen)) out[idx] = toInt16(acc[i]);
            }
            if (shift >= static_cast<int64_t>(accLen)) {
                std::memset(acc, 0, accLen * sizeof(float));
            } else {
                std::memmove(acc, acc + shift, (accLen - shift) * sizeof(float));
                std::memset(acc + (accLen - shift), 0, shift * sizeof(float));
            }
            accBase = n0;
        }

        // ── windowed input block -> spectrum ──
        std::memset(fbuf, 0, static_cast<size_t>(block) * 2 * sizeof(float));
        for (uint32_t i = 0; i < block; ++i) {
            const int64_t src = p0 + static_cast<int64_t>(i);
            if (src < 0 || src >= static_cast<int64_t>(inCount)) continue;
            fbuf[2 * i] = in[src] * win[i];
        }
        if (!fft.forward(fbuf, block)) { ok = false; break; }

        // ── band-limit + spectral zero-pad to padded, then back to time ──
        std::memset(ybuf, 0, static_cast<size_t>(padded) * 2 * sizeof(float));
        const uint32_t half = block / 2;
        for (uint32_t k = 0; k < half; ++k) {
            if (k > cutoff) break;
            ybuf[2 * k]     = fbuf[2 * k];
            ybuf[2 * k + 1] = fbuf[2 * k + 1];
        }
        for (uint32_t k = half + 1; k < block; ++k) {
            if (block - k > cutoff) continue;
            const uint32_t dst = padded - (block - k);
            ybuf[2 * dst]     = fbuf[2 * k];
            ybuf[2 * dst + 1] = fbuf[2 * k + 1];
        }
        if (cutoff >= half) {
            // Split the Nyquist bin so the padded block stays real and matches
            // the original samples exactly on the original grid.
            ybuf[2 * half]               = 0.5f * fbuf[2 * half];
            ybuf[2 * half + 1]           = 0.5f * fbuf[2 * half + 1];
            ybuf[2 * (padded - half)]      = 0.5f * fbuf[2 * half];
            ybuf[2 * (padded - half) + 1]  = -0.5f * fbuf[2 * half + 1];
        }
        if (!fft.inverse(ybuf, padded)) { ok = false; break; }

        // De-interleave in place: the padded block, at inRate * kOversample.
        const float gain = static_cast<float>(kOversample);
        for (uint32_t i = 0; i < padded; ++i) ybuf[i] = ybuf[2 * i] * gain;

        // ── read the block at the output grid ──
        for (int64_t n = n0; n < n1; ++n) {
            const double t = static_cast<double>(n) / ratio;      // input-sample time
            const double u = (t - static_cast<double>(p0)) * kOversample;
            if (u < 0.0 || u >= static_cast<double>(padded)) continue;
            const int64_t ui = static_cast<int64_t>(u);
            const float frac = static_cast<float>(u - static_cast<double>(ui));
            const int64_t slot = n - accBase;
            if (slot < 0 || slot >= static_cast<int64_t>(accLen)) continue;
            acc[slot] += hermite(ybuf, padded, ui, frac);
        }
    }

    if (ok) {
        for (int64_t i = 0; i < static_cast<int64_t>(accLen); ++i) {
            const int64_t idx = accBase + i;
            if (idx < 0) continue;
            if (idx >= static_cast<int64_t>(outCount)) break;
            out[idx] = toInt16(acc[i]);
        }
    }

    scratchFree(mem);
    return ok;
}

} // namespace crosspad

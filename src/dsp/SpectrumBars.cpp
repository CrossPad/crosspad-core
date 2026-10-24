// SPDX-License-Identifier: GPL-3.0-or-later

#include "crosspad/dsp/SpectrumBars.hpp"

#include <cmath>
#include <cstring>

namespace crosspad::dsp {

namespace {

constexpr float kPi = 3.14159265358979f;
/// 20 * log10(2): dB of one doubling of amplitude.
constexpr float kDbPerOctave = 6.0205999f;

float barEdgeHz(uint32_t edge, uint32_t bars) {
    return kSpectrumLowHz * powf(kSpectrumHighHz / kSpectrumLowHz,
                                 static_cast<float>(edge) / static_cast<float>(bars));
}

/// log2 of a positive finite float: the exponent plus a 4th-order fit of the
/// mantissa on [1, 2), good to ~1e-4. log10f was ~3 us a bar on the S3, a
/// third of the frame; the scale only needs a hundredth of a dB.
float fastLog2(float x) {
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    const float e = static_cast<float>(static_cast<int32_t>(bits >> 23) - 127);
    bits = (bits & 0x007FFFFFu) | 0x3F800000u;
    float m;
    memcpy(&m, &bits, sizeof(m));
    return e + (-1.7417939f + (2.8212026f + (-1.4699568f + (0.44717955f - 0.056570851f * m) * m) * m) * m);
}

float powerAt(const float* fft, uint32_t bin) {
    const float re = fft[2 * bin];
    const float im = fft[2 * bin + 1];
    return re * re + im * im;
}

} // namespace

void spectrumHann(float* w, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        w[i] = 0.5f - 0.5f * cosf(2.0f * kPi * static_cast<float>(i) / static_cast<float>(n));
    }
}

void spectrumMapBars(SpectrumBar* out, uint32_t bars, uint32_t n, uint32_t sampleRate) {
    const float binsPerHz = static_cast<float>(n) / static_cast<float>(sampleRate);
    const float lastBin   = static_cast<float>(n / 2 - 1);
    for (uint32_t i = 0; i < bars; ++i) {
        float a = barEdgeHz(i, bars) * binsPerHz;
        float b = barEdgeHz(i + 1, bars) * binsPerHz;
        if (a > lastBin) a = lastBin;
        if (b > lastBin) b = lastBin;
        if (b - a < 1.0f) {
            const float c = 0.5f * (a + b);
            uint32_t lo = static_cast<uint32_t>(c);
            if (lo + 1 > static_cast<uint32_t>(lastBin)) lo = static_cast<uint32_t>(lastBin) - 1;
            out[i] = SpectrumBar{ static_cast<uint16_t>(lo), static_cast<uint16_t>(lo + 1),
                                  c - static_cast<float>(lo), true };
        } else {
            out[i] = SpectrumBar{ static_cast<uint16_t>(ceilf(a)), static_cast<uint16_t>(floorf(b)),
                                  0.0f, false };
        }
    }
}

float spectrumBarHz(uint32_t i, uint32_t bars) {
    return sqrtf(barEdgeHz(i, bars) * barEdgeHz(i + 1, bars));
}

void spectrumLevels(const float* fft, uint32_t n, const SpectrumBar* bars,
                     uint32_t barCount, float* levels) {
    // |X| of a Hann-windowed sine of amplitude A peaks at A * n / 4.
    const float toAmplitude = 4.0f / static_cast<float>(n);
    for (uint32_t i = 0; i < barCount; ++i) {
        const SpectrumBar& bar = bars[i];
        float mag;
        if (bar.interpolate) {
            const float m0 = sqrtf(powerAt(fft, bar.lo));
            const float m1 = sqrtf(powerAt(fft, bar.hi));
            mag = m0 + (m1 - m0) * bar.frac;
        } else {
            float p = 0.0f;
            for (uint32_t k = bar.lo; k <= bar.hi; ++k) {
                const float pk = powerAt(fft, k);
                if (pk > p) p = pk;
            }
            mag = sqrtf(p);
        }
        mag *= toAmplitude;
        float norm = 0.0f;
        if (mag > 0.0f) {
            norm = (kDbPerOctave * fastLog2(mag) - kSpectrumFloorDb) / -kSpectrumFloorDb;
            if (norm < 0.0f) norm = 0.0f;
            if (norm > 1.0f) norm = 1.0f;
        }
        levels[i] = norm;
    }
}

} // namespace crosspad::dsp

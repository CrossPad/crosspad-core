// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file SpectrumBars.hpp
 * @brief A spectrum analyser's bars from an FFT: window, log-frequency mapping, dB scale.
 *
 * A Hann-windowed FFT of the newest kSpectrumFftSize samples is reduced to
 * bars on a log-frequency axis and a dB scale. Pure functions over
 * caller-owned buffers — no state, no allocation; the FFT itself is the
 * caller's (crosspad::createFft()).
 */

#include <cstdint>

namespace crosspad::dsp {

/// Transform length. 43 Hz per bin at 44.1 kHz.
inline constexpr uint32_t kSpectrumFftSize = 1024;
/// Most bars the mapping accepts.
inline constexpr uint32_t kSpectrumMaxBars = 64;
/// Left and right edge of the frequency axis.
inline constexpr float kSpectrumLowHz  = 30.0f;
inline constexpr float kSpectrumHighHz = 20000.0f;
/// Bottom of the dB scale; 0 dBFS (a full-scale sine) is the top.
inline constexpr float kSpectrumFloorDb = -72.0f;

/**
 * @brief Which FFT bins one bar reads.
 *
 * A bar wider than a bin takes the loudest bin in [lo, hi]. A narrower one
 * (the low end of a log axis) would repeat the same bin over several bars, so
 * it interpolates the magnitude between bin lo and lo + 1 at @c frac instead.
 */
struct SpectrumBar {
    uint16_t lo;
    uint16_t hi;
    float    frac;
    bool     interpolate;
};

/// Hann window of @p n points, periodic form (what an FFT wants).
void spectrumHann(float* w, uint32_t n);

/// Map @p bars bars onto the bins of an @p n point FFT of a @p sampleRate stream.
void spectrumMapBars(SpectrumBar* out, uint32_t bars, uint32_t n, uint32_t sampleRate);

/// Centre frequency of bar @p i of @p bars, in Hz.
float spectrumBarHz(uint32_t i, uint32_t bars);

/**
 * @brief Bar levels, 0 (floor) to 1 (0 dBFS), from a transformed block.
 *
 * @param fft interleaved [re, im] pairs of a Hann-windowed block of @p n points.
 *            A full-scale sine reads 1.0.
 */
void spectrumLevels(const float* fft, uint32_t n, const SpectrumBar* bars,
                    uint32_t barCount, float* levels);

} // namespace crosspad::dsp

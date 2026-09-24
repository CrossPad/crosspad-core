// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file PitchDetector.hpp
 * @brief Root-note detection for a recorded instrument zone.
 *
 * Load-time code: it allocates its own working buffers (~56 kB) on the heap
 * and frees them before returning. Never call it from the audio thread.
 */

#include "crosspad/dsp/IFft.hpp"

#include <cstdint>

namespace crosspad {

struct PitchResult {
    float hz = 0.0f;          ///< Estimated fundamental in Hz.
    float midi = 0.0f;        ///< Fractional MIDI note, 69 + 12*log2(hz/440).
    float confidence = 0.0f;  ///< Peak magnitude over the mean magnitude.
};

/// Analysis window, in samples. Inputs shorter than this are zero-padded.
inline constexpr uint32_t kPitchWindow = 4096;

/// Floats of working memory detectPitch() uses: the windowed frame, the
/// interleaved complex spectrum and its magnitudes.
inline constexpr uint32_t kPitchScratchFloats = kPitchWindow + 2 * kPitchWindow + kPitchWindow / 2 + 1;

/**
 * @brief Estimate the fundamental of a monophonic 16-bit PCM buffer.
 *
 * Picks the loudest kPitchWindow-sample frame (RMS, step kPitchWindow/4,
 * skipping the first 20 ms so the attack transient does not win), Hann
 * windows it, takes its spectrum through @p fft, scores candidate
 * fundamentals with a weighted harmonic product spectrum (kHarmonics terms) and
 * interpolates the peak.
 *
 * @param scratch kPitchScratchFloats floats of working memory, or nullptr to
 *        take them from the heap (~56 kB) for the duration of the call.
 * @return false for a null/empty buffer, a zero sample rate, or silence.
 */
bool detectPitch(IFft& fft, const int16_t* pcm, uint32_t count, uint32_t sampleRate,
                 PitchResult& out, float* scratch = nullptr);

} // namespace crosspad

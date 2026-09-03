// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file IFft.hpp
 * @brief Complex FFT seam.
 *
 * Buffers are interleaved [re, im] pairs, `n` complex points, transformed in
 * place. `n` must be a power of two in [kMinFftSize, kMaxFftSize].
 *
 * The seam exists so the portable radix-2 implementation (PC, unit tests) and
 * an ESP-DSP backed one (`EspDspFft` in crosspad-platform-idf) are
 * interchangeable. Both are load-time code: nothing in the real-time audio
 * path calls an FFT.
 */

#include <cstdint>

namespace crosspad {

/// Smallest transform an IFft implementation must accept.
inline constexpr uint32_t kMinFftSize = 64;
/// Largest transform an IFft implementation must accept.
inline constexpr uint32_t kMaxFftSize = 8192;

/// True when `n` is a power of two inside the supported range.
inline bool fftSizeValid(uint32_t n) {
    return n >= kMinFftSize && n <= kMaxFftSize && (n & (n - 1)) == 0;
}

class IFft {
public:
    virtual ~IFft() = default;

    /// Forward transform, in place. Returns false for an unsupported size.
    virtual bool forward(float* data, uint32_t n) = 0;

    /// Inverse transform, in place, including the 1/n scale.
    virtual bool inverse(float* data, uint32_t n) = 0;

    /**
     * @brief Largest `n` this implementation accepts, a power of two.
     *
     * Not every backend reaches kMaxFftSize: esp-dsp builds one twiddle table
     * for a size fixed at *its* build time, and a prebuilt Arduino sdkconfig
     * may cap it at 4096. Callers that pick a transform size -- FftResampler
     * sizes its analysis block off this -- must read it rather than assume
     * kMaxFftSize.
     */
    virtual uint32_t maxSize() const = 0;
};

} // namespace crosspad

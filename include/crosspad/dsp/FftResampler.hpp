// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file FftResampler.hpp
 * @brief Band-limited sample-rate conversion for whole zones, at load time.
 *
 * Overlap-add over N-sample input blocks at 50 % hop with a periodic Hann
 * analysis window and rectangular synthesis — that pair is COLA at 50 %
 * (w(n) + w(n + N/2) == 1 exactly), so a constant reconstructs to 1.0 without
 * any normalisation pass.
 *
 * Each block goes forward through @p fft, is brick-wall limited to
 * min(inRate, outRate)/2, spectrally zero-padded to 2x (an exact, alias-free
 * oversample), and read back at the output grid with cubic Hermite
 * interpolation. Arbitrary ratios such as 48000 -> 44100 are not a power of
 * two, so the last step cannot be a plain decimation; doing it on the 2x
 * oversampled block keeps the interpolation error far below the 16-bit floor.
 *
 * **N is not fixed**: the inverse transform runs at 2N, so the block is
 * min(4096, fft.maxSize() / 2) -- an esp-dsp built for 4096 gives 2048-sample
 * blocks, an 8192 one gives 4096. Output is identical either way (the tone,
 * the length and the 16-bit floor all match); the smaller block only costs
 * more overlap-add passes and less scratch. A backend whose maxSize() cannot
 * even reach 2 * kMinFftSize is refused.
 *
 * All working memory comes from the caller's allocator and is released before
 * returning -- one allocation, but not a constant size:
 *
 *     floats = 2N (forward FFT buffer)
 *            + 4N (2x zero-padded spectrum / block)
 *            + N  (Hann window)
 *            + N * outRate/inRate + 8   (output accumulator)
 *
 * At N = 4096 the fixed part is 28672 floats (112 KiB) and the accumulator
 * scales with the ratio. Upsampling is the expensive direction: 8 k -> 44.1 k
 * needs ~200 KiB in total, 48 k -> 44.1 k ~127 KiB. Budget for the upsample,
 * not the average.
 */

#include "crosspad/dsp/IFft.hpp"

#include <cstddef>
#include <cstdint>

namespace crosspad {

/// Frame count the conversion produces: round(inCount * outRate / inRate).
uint32_t resampledLength(uint32_t inCount, uint32_t inRate, uint32_t outRate);

/**
 * @brief Convert @p in from @p inRate to @p outRate, writing @p outCount frames.
 *
 * @param scratchAlloc allocator for working memory (PSRAM on the device)
 * @param scratchFree  matching deallocator
 * @return false on bad arguments or a failed allocation.
 */
bool fftResample(IFft& fft, const int16_t* in, uint32_t inCount, uint32_t inRate,
                 int16_t* out, uint32_t outCount, uint32_t outRate,
                 void* (*scratchAlloc)(size_t), void (*scratchFree)(void*));

} // namespace crosspad

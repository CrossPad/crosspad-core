// SPDX-License-Identifier: GPL-3.0-or-later
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
 * oversample; FftResamplerStream can go further, see its begin()), and read
 * back at the output grid with cubic Hermite interpolation. Arbitrary ratios such as 48000 -> 44100 are not a power of
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
 * not the average. FftResamplerStream adds one more block (N floats) for the
 * input it assembles, and in exchange never sees the signal it is converting.
 */

#include "crosspad/dsp/IFft.hpp"

#include <cstddef>
#include <cstdint>

namespace crosspad {

/// Frame count the conversion produces: round(inCount * outRate / inRate).
uint32_t resampledLength(uint32_t inCount, uint32_t inRate, uint32_t outRate);

/**
 * @brief The same conversion, fed a piece at a time.
 *
 * The algorithm above, with its state named rather than held on the stack of
 * one call: nothing here ever sees the whole signal, so a zone longer than the
 * memory budget can be read, converted and written straight into its final
 * buffer. fftResample() is a push() of everything plus a flush(), so there is
 * one implementation of the maths and both forms produce the same samples.
 *
 * Working memory is one allocation from begin(), block-sized (the fixed part
 * of the budget above plus one analysis block for the input), and is released
 * by end() or the destructor.
 *
 * Output is positional and monotonic: successive calls continue where the last
 * one stopped. **Whatever does not fit @p outCapacity is discarded**, which is
 * what makes a budget-truncated load a truncation rather than a corruption —
 * a caller that wants everything either sizes each destination to
 * resampledLength(n, inRate, outRate) + kStreamOutSlack or stops pushing once
 * a call comes back short.
 */
class FftResamplerStream {
public:
    /// Frames a push() may emit beyond the ratio, from block alignment.
    static constexpr uint32_t kStreamOutSlack = 8;

    FftResamplerStream() = default;
    ~FftResamplerStream() { end(); }

    FftResamplerStream(const FftResamplerStream&) = delete;
    FftResamplerStream& operator=(const FftResamplerStream&) = delete;

    /**
     * @brief Size the blocks and take the scratch.
     *
     * @p oversample is the spectral zero-pad factor the block is read back at
     * (a power of two, 2 or more). The cubic read-out is the whole of the
     * error above a few kHz, and each doubling takes it ~20 dB down: at 48 k
     * -> 44.1 k a 19 kHz tone leaves -30 dB of images at 2, -50 at 4, -69 at
     * 8 (test_KitRateConvert). The inverse transform grows with it, so the
     * block shrinks to keep it inside fft.maxSize() -- which changes nothing
     * audible -- and the scratch grows by 2N floats a doubling.
     *
     * @return false on a zero rate, a backend too small for a block, a missing
     *         allocator, an oversample that is not a power of two, or a
     *         failed allocation.
     */
    bool begin(IFft& fft, uint32_t inRate, uint32_t outRate,
               void* (*alloc)(size_t), void (*free)(void*),
               uint32_t oversample = 2);

    /// @return frames written to @p out (never more than @p outCapacity).
    uint32_t push(const int16_t* in, uint32_t n, int16_t* out, uint32_t outCapacity);

    /**
     * @brief Finish the signal: the tail is analysed against zeros, as the
     *        whole-buffer form does past its last input sample.
     * @return frames written. push() after this does nothing until begin().
     */
    uint32_t flush(int16_t* out, uint32_t outCapacity);

    /// False once a transform has failed; the stream then produces nothing.
    bool ok() const { return ok_; }

    void end();

private:
    /// One analysis block at @p p0, already assembled in inBuf_.
    void processBlock(int16_t* out, uint32_t outCapacity, uint32_t& written);
    /// Move the accumulator up to @p n0, emitting what that finishes.
    void slideTo(int64_t n0, int16_t* out, uint32_t outCapacity, uint32_t& written);

    IFft* fft_ = nullptr;
    void* (*alloc_)(size_t) = nullptr;
    void (*free_)(void*) = nullptr;
    float* mem_ = nullptr;
    float* fbuf_ = nullptr;   ///< forward FFT buffer, 2 * block_ floats
    float* ybuf_ = nullptr;   ///< zero-padded spectrum / oversampled block
    float* win_ = nullptr;    ///< Hann analysis window
    float* acc_ = nullptr;    ///< output accumulator
    float* inBuf_ = nullptr;  ///< the analysis block being assembled

    uint32_t block_ = 0;
    uint32_t hop_ = 0;
    uint32_t padded_ = 0;
    uint32_t accLen_ = 0;
    uint32_t cutoff_ = 0;
    uint32_t inFill_ = 0;     ///< samples of inBuf_ that are assembled
    uint32_t oversample_ = 2;
    double ratio_ = 1.0;

    int64_t p0_ = 0;          ///< input position of the block being assembled
    int64_t accBase_ = 0;     ///< output frame acc_[0] stands for
    int64_t accEnd_ = 0;      ///< highest output frame any block has touched
    uint64_t inTotal_ = 0;    ///< input frames pushed so far
    bool flushed_ = false;
    bool ok_ = false;
};

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

// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/dsp/FftResampler.hpp"
#include "crosspad/dsp/DspMath.hpp"

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

bool FftResamplerStream::begin(IFft& fft, uint32_t inRate, uint32_t outRate,
                               void* (*alloc)(size_t), void (*free)(void*)) {
    end();
    if (!alloc || !free) return false;
    if (inRate == 0 || outRate == 0) return false;

    const uint32_t block = blockFor(fft);
    if (block == 0) return false;

    block_ = block;
    hop_ = block / 2;                        // 50 % overlap
    padded_ = block * kOversample;
    ratio_ = static_cast<double>(outRate) / static_cast<double>(inRate);

    // Accumulator spans one block's worth of output plus interpolation slack.
    accLen_ = static_cast<uint32_t>(block_ * ratio_) + kStreamOutSlack;
    const size_t floats = static_cast<size_t>(block_) * 2    // forward FFT buffer
                        + static_cast<size_t>(padded_) * 2   // zero-padded spectrum / block
                        + block_                             // Hann window
                        + accLen_                            // output accumulator
                        + block_;                            // the block being assembled
    mem_ = static_cast<float*>(alloc(floats * sizeof(float)));
    if (!mem_) {
        block_ = 0;
        return false;
    }
    alloc_ = alloc;
    free_ = free;
    fft_ = &fft;

    fbuf_ = mem_;
    ybuf_ = fbuf_ + static_cast<size_t>(block_) * 2;
    win_  = ybuf_ + static_cast<size_t>(padded_) * 2;
    acc_  = win_ + block_;
    inBuf_ = acc_ + accLen_;

    for (uint32_t i = 0; i < block_; ++i) {
        win_[i] = 0.5f - 0.5f * std::cos(2.0f * static_cast<float>(kPi)
                                       * static_cast<float>(i) / static_cast<float>(block_));
    }
    std::memset(acc_, 0, accLen_ * sizeof(float));

    // Anti-alias cutoff, in bins of the block-point spectrum.
    const uint32_t nyq = (outRate < inRate) ? outRate : inRate;
    cutoff_ = static_cast<uint32_t>(static_cast<double>(block_) * nyq / (2.0 * inRate));
    if (cutoff_ > block_ / 2) cutoff_ = block_ / 2;

    // The first analysis block straddles the start of the signal, exactly as
    // the whole-buffer form's b == -1 pass does: its leading hop samples are
    // the silence before sample 0.
    std::memset(inBuf_, 0, block_ * sizeof(float));
    inFill_ = hop_;
    p0_ = -static_cast<int64_t>(hop_);
    accBase_ = 0;
    accEnd_ = 0;
    inTotal_ = 0;
    flushed_ = false;
    ok_ = true;
    return true;
}

void FftResamplerStream::end() {
    if (mem_ && free_) free_(mem_);
    mem_ = nullptr;
    fbuf_ = ybuf_ = win_ = acc_ = inBuf_ = nullptr;
    fft_ = nullptr;
    alloc_ = nullptr;
    free_ = nullptr;
    block_ = hop_ = padded_ = accLen_ = cutoff_ = inFill_ = 0;
    ratio_ = 1.0;
    p0_ = accBase_ = accEnd_ = 0;
    inTotal_ = 0;
    flushed_ = false;
    ok_ = false;
}

void FftResamplerStream::slideTo(int64_t n0, int16_t* out, uint32_t outCapacity,
                                 uint32_t& written) {
    if (n0 <= accBase_) return;
    const int64_t shift = n0 - accBase_;
    for (int64_t i = 0; i < shift; ++i) {
        // Past accLen_ the accumulator has nothing to say about that frame --
        // it is silence, and it still has to be emitted or every later frame
        // would land one position early.
        const int16_t v = (i < static_cast<int64_t>(accLen_)) ? toInt16(acc_[i]) : 0;
        if (written < outCapacity) out[written++] = v;
    }
    if (shift >= static_cast<int64_t>(accLen_)) {
        std::memset(acc_, 0, accLen_ * sizeof(float));
    } else {
        std::memmove(acc_, acc_ + shift, (accLen_ - shift) * sizeof(float));
        std::memset(acc_ + (accLen_ - shift), 0, static_cast<size_t>(shift) * sizeof(float));
    }
    accBase_ = n0;
}

void FftResamplerStream::processBlock(int16_t* out, uint32_t outCapacity, uint32_t& written) {
    const int64_t p0 = p0_;
    int64_t n0 = ceilDouble(static_cast<double>(p0) * ratio_);
    const int64_t n1 = ceilDouble(static_cast<double>(p0 + block_) * ratio_);
    if (n0 < 0) n0 = 0;
    if (n1 <= n0) return;

    slideTo(n0, out, outCapacity, written);

    // ── windowed input block -> spectrum ──
    std::memset(fbuf_, 0, static_cast<size_t>(block_) * 2 * sizeof(float));
    for (uint32_t i = 0; i < block_; ++i) fbuf_[2 * i] = inBuf_[i] * win_[i];
    if (!fft_->forward(fbuf_, block_)) { ok_ = false; return; }

    // ── band-limit + spectral zero-pad to padded_, then back to time ──
    std::memset(ybuf_, 0, static_cast<size_t>(padded_) * 2 * sizeof(float));
    const uint32_t half = block_ / 2;
    for (uint32_t k = 0; k < half; ++k) {
        if (k > cutoff_) break;
        ybuf_[2 * k]     = fbuf_[2 * k];
        ybuf_[2 * k + 1] = fbuf_[2 * k + 1];
    }
    for (uint32_t k = half + 1; k < block_; ++k) {
        if (block_ - k > cutoff_) continue;
        const uint32_t dst = padded_ - (block_ - k);
        ybuf_[2 * dst]     = fbuf_[2 * k];
        ybuf_[2 * dst + 1] = fbuf_[2 * k + 1];
    }
    if (cutoff_ >= half) {
        // Split the Nyquist bin so the padded block stays real and matches
        // the original samples exactly on the original grid.
        ybuf_[2 * half]                = 0.5f * fbuf_[2 * half];
        ybuf_[2 * half + 1]            = 0.5f * fbuf_[2 * half + 1];
        ybuf_[2 * (padded_ - half)]     = 0.5f * fbuf_[2 * half];
        ybuf_[2 * (padded_ - half) + 1] = -0.5f * fbuf_[2 * half + 1];
    }
    if (!fft_->inverse(ybuf_, padded_)) { ok_ = false; return; }

    // De-interleave in place: the padded block, at inRate * kOversample.
    const float gain = static_cast<float>(kOversample);
    for (uint32_t i = 0; i < padded_; ++i) ybuf_[i] = ybuf_[2 * i] * gain;

    // ── read the block at the output grid ──
    for (int64_t n = n0; n < n1; ++n) {
        const double t = static_cast<double>(n) / ratio_;      // input-sample time
        const double u = (t - static_cast<double>(p0)) * kOversample;
        if (u < 0.0 || u >= static_cast<double>(padded_)) continue;
        const int64_t ui = static_cast<int64_t>(u);
        const float frac = static_cast<float>(u - static_cast<double>(ui));
        const int64_t slot = n - accBase_;
        if (slot < 0 || slot >= static_cast<int64_t>(accLen_)) continue;
        acc_[slot] += hermite(ybuf_, padded_, ui, frac);
    }
    if (n1 > accEnd_) accEnd_ = n1;
}

uint32_t FftResamplerStream::push(const int16_t* in, uint32_t n, int16_t* out,
                                  uint32_t outCapacity) {
    uint32_t written = 0;
    if (!ok_ || flushed_ || !in || n == 0) return 0;
    if (!out) outCapacity = 0;

    uint32_t consumed = 0;
    while (consumed < n) {
        uint32_t take = block_ - inFill_;
        if (take > n - consumed) take = n - consumed;
        for (uint32_t i = 0; i < take; ++i) inBuf_[inFill_ + i] = in[consumed + i];
        inFill_ += take;
        consumed += take;
        inTotal_ += take;
        if (inFill_ < block_) break;

        processBlock(out, outCapacity, written);
        if (!ok_) break;
        std::memmove(inBuf_, inBuf_ + hop_, (block_ - hop_) * sizeof(float));
        inFill_ = block_ - hop_;
        p0_ += static_cast<int64_t>(hop_);
    }
    return written;
}

uint32_t FftResamplerStream::flush(int16_t* out, uint32_t outCapacity) {
    uint32_t written = 0;
    if (!ok_ || flushed_) return 0;
    if (!out) outCapacity = 0;
    flushed_ = true;

    // Every block the whole-buffer form would still run: it iterates while the
    // block start is inside the signal, reading zeros past its end.
    while (p0_ < static_cast<int64_t>(inTotal_)) {
        for (uint32_t i = inFill_; i < block_; ++i) inBuf_[i] = 0.0f;
        inFill_ = block_;
        processBlock(out, outCapacity, written);
        if (!ok_) return written;
        std::memmove(inBuf_, inBuf_ + hop_, (block_ - hop_) * sizeof(float));
        inFill_ = block_ - hop_;
        p0_ += static_cast<int64_t>(hop_);
    }

    for (int64_t i = 0; i < static_cast<int64_t>(accLen_) && accBase_ + i < accEnd_; ++i) {
        if (written < outCapacity) out[written++] = toInt16(acc_[i]);
    }
    return written;
}

bool fftResample(IFft& fft, const int16_t* in, uint32_t inCount, uint32_t inRate,
                 int16_t* out, uint32_t outCount, uint32_t outRate,
                 void* (*scratchAlloc)(size_t), void (*scratchFree)(void*)) {
    if (!in || !out || !scratchAlloc || !scratchFree) return false;
    if (inCount == 0 || inRate == 0 || outRate == 0 || outCount == 0) return false;

    FftResamplerStream st;
    if (!st.begin(fft, inRate, outRate, scratchAlloc, scratchFree)) return false;

    std::memset(out, 0, static_cast<size_t>(outCount) * sizeof(int16_t));
    uint32_t written = st.push(in, inCount, out, outCount);
    written += st.flush(out + written, outCount - written);
    const bool ok = st.ok();
    st.end();
    return ok;
}

} // namespace crosspad

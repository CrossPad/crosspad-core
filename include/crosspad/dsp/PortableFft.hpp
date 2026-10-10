// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file PortableFft.hpp
 * @brief Scalar iterative radix-2 FFT — the portable IFft implementation.
 *
 * Twiddle factors are built lazily, once per transform size, and kept for the
 * lifetime of the instance. That allocation happens on the first transform of
 * a given size; every caller is load-time code, so it is not an RT concern.
 * Table construction is guarded by a mutex, so one instance may be shared.
 *
 * The default instance reaches kMaxFftSize. A smaller cap can be asked for --
 * that exists so a host build can stand in for a backend whose table is
 * smaller (esp-dsp built for 4096), and exercise the size negotiation
 * FftResampler does through IFft::maxSize().
 */

#include "crosspad/dsp/IFft.hpp"

#include <mutex>
#include <vector>

namespace crosspad {

class PortableFft : public IFft {
public:
    /// @param maxSize largest transform to accept; rounded down to a power of
    ///        two in [kMinFftSize, kMaxFftSize].
    explicit PortableFft(uint32_t maxSize = kMaxFftSize);

    bool forward(float* data, uint32_t n) override;
    bool inverse(float* data, uint32_t n) override;
    uint32_t maxSize() const override { return maxSize_; }

private:
    /// 64, 128, ... 8192 — one slot per supported size.
    static constexpr int kNumSizes = 8;

    /// Interleaved [cos, sin] of exp(-2*pi*i*k/n) for k in [0, n/2).
    const float* twiddles(uint32_t n);
    void transform(float* data, uint32_t n, const float* tw);

    uint32_t maxSize_ = kMaxFftSize;
    std::vector<float> tables_[kNumSizes];
    std::mutex tableMutex_;
};

} // namespace crosspad

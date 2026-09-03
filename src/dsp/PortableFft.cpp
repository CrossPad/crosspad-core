// SPDX-License-Identifier: MIT
#include "crosspad/dsp/PortableFft.hpp"

#include <cmath>

namespace crosspad {

namespace {

int sizeSlot(uint32_t n) {
    int slot = -1;
    for (uint32_t s = kMinFftSize, i = 0; s <= kMaxFftSize; s <<= 1, ++i) {
        if (s == n) { slot = static_cast<int>(i); break; }
    }
    return slot;
}

/// In-place bit-reversal permutation of interleaved complex pairs.
void bitReverse(float* data, uint32_t n) {
    uint32_t j = 0;
    for (uint32_t i = 1; i < n; ++i) {
        uint32_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = data[2 * i];     float ti = data[2 * i + 1];
            data[2 * i]     = data[2 * j];
            data[2 * i + 1] = data[2 * j + 1];
            data[2 * j]     = tr;
            data[2 * j + 1] = ti;
        }
    }
}

/// Largest power of two in [kMinFftSize, kMaxFftSize] that is <= @p want.
uint32_t clampToPowerOfTwo(uint32_t want) {
    if (want >= kMaxFftSize) return kMaxFftSize;
    uint32_t s = kMinFftSize;
    while ((s << 1) <= want && (s << 1) <= kMaxFftSize) s <<= 1;
    return s;
}

} // namespace

PortableFft::PortableFft(uint32_t maxSize) : maxSize_(clampToPowerOfTwo(maxSize)) {}

const float* PortableFft::twiddles(uint32_t n) {
    const int slot = sizeSlot(n);
    if (slot < 0) return nullptr;

    std::lock_guard<std::mutex> lock(tableMutex_);
    std::vector<float>& t = tables_[slot];
    if (t.empty()) {
        t.resize(n);  // n/2 complex entries
        const double step = -2.0 * M_PI / static_cast<double>(n);
        for (uint32_t k = 0; k < n / 2; ++k) {
            t[2 * k]     = static_cast<float>(std::cos(step * k));
            t[2 * k + 1] = static_cast<float>(std::sin(step * k));
        }
    }
    return t.data();
}

void PortableFft::transform(float* data, uint32_t n, const float* tw) {
    bitReverse(data, n);
    for (uint32_t len = 2; len <= n; len <<= 1) {
        const uint32_t half = len >> 1;
        const uint32_t step = n / len;
        for (uint32_t base = 0; base < n; base += len) {
            for (uint32_t j = 0; j < half; ++j) {
                const float wr = tw[2 * (j * step)];
                const float wi = tw[2 * (j * step) + 1];
                float* a = data + 2 * (base + j);
                float* b = data + 2 * (base + j + half);
                const float vr = b[0] * wr - b[1] * wi;
                const float vi = b[0] * wi + b[1] * wr;
                b[0] = a[0] - vr;
                b[1] = a[1] - vi;
                a[0] += vr;
                a[1] += vi;
            }
        }
    }
}

bool PortableFft::forward(float* data, uint32_t n) {
    if (!data || !fftSizeValid(n) || n > maxSize_) return false;
    const float* tw = twiddles(n);
    if (!tw) return false;
    transform(data, n, tw);
    return true;
}

bool PortableFft::inverse(float* data, uint32_t n) {
    if (!data || !fftSizeValid(n) || n > maxSize_) return false;
    const float* tw = twiddles(n);
    if (!tw) return false;

    // conj -> forward -> conj, then the 1/n scale the contract promises.
    for (uint32_t i = 0; i < n; ++i) data[2 * i + 1] = -data[2 * i + 1];
    transform(data, n, tw);
    const float scale = 1.0f / static_cast<float>(n);
    for (uint32_t i = 0; i < n; ++i) {
        data[2 * i]     *=  scale;
        data[2 * i + 1] *= -scale;
    }
    return true;
}

} // namespace crosspad

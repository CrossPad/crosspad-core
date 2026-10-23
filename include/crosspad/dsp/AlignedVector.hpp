// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file AlignedVector.hpp
 * @brief A std::vector whose data starts on 16 bytes: what the S3's 128-bit
 *        float loads in dsp/AudioKernels.hpp need to take a buffer at all.
 */

#include <cstddef>
#include <new>
#include <vector>
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

namespace crosspad::dsp {

constexpr std::size_t kSimdAlign = 16;

template <class T>
struct AlignedAllocator {
    using value_type = T;

    AlignedAllocator() = default;
    template <class U>
    AlignedAllocator(const AlignedAllocator<U>&) noexcept {}

#if defined(ESP_PLATFORM)
    /* Internal RAM first: these are the render's per-block buffers, read and
     * written every 1.45 ms. In PSRAM each access went through the data cache
     * both cores share, and core 0's card and UI traffic evicted them between
     * passes -- a 128-float peak scan measured ~63 cycles a float. */
    T* allocate(std::size_t n)
    {
        void* p = heap_caps_aligned_alloc(kSimdAlign, n * sizeof(T), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!p) p = heap_caps_aligned_alloc(kSimdAlign, n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept { heap_caps_free(p); }
#else
    T* allocate(std::size_t n)
    {
        return static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t{kSimdAlign}));
    }
    void deallocate(T* p, std::size_t) noexcept { ::operator delete(p, std::align_val_t{kSimdAlign}); }
#endif

    template <class U>
    bool operator==(const AlignedAllocator<U>&) const noexcept { return true; }
    template <class U>
    bool operator!=(const AlignedAllocator<U>&) const noexcept { return false; }
};

template <class T>
using AlignedVector = std::vector<T, AlignedAllocator<T>>;

} // namespace crosspad::dsp

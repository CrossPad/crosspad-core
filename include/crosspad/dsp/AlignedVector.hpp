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

namespace crosspad::dsp {

constexpr std::size_t kSimdAlign = 16;

template <class T>
struct AlignedAllocator {
    using value_type = T;

    AlignedAllocator() = default;
    template <class U>
    AlignedAllocator(const AlignedAllocator<U>&) noexcept {}

    T* allocate(std::size_t n)
    {
        return static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t{kSimdAlign}));
    }
    void deallocate(T* p, std::size_t) noexcept { ::operator delete(p, std::align_val_t{kSimdAlign}); }

    template <class U>
    bool operator==(const AlignedAllocator<U>&) const noexcept { return true; }
    template <class U>
    bool operator!=(const AlignedAllocator<U>&) const noexcept { return false; }
};

template <class T>
using AlignedVector = std::vector<T, AlignedAllocator<T>>;

} // namespace crosspad::dsp

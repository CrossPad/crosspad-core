// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace crosspad {

enum class AudioFormat : uint8_t {
    Float32 = 1 << 0,
    Int32   = 1 << 1,
    Int16   = 1 << 2,
};

constexpr uint32_t audioFormatMask(AudioFormat f) {
    return static_cast<uint32_t>(f);
}

constexpr bool audioFormatSupports(uint32_t mask, AudioFormat f) {
    return (mask & audioFormatMask(f)) != 0;
}

/// Pick the highest-quality format the stream supports.
/// Order: Float32 > Int32 > Int16. Returns Int16 if mask is empty (safe fallback).
constexpr AudioFormat audioFormatPreferred(uint32_t mask) {
    if (audioFormatSupports(mask, AudioFormat::Float32)) return AudioFormat::Float32;
    if (audioFormatSupports(mask, AudioFormat::Int32))   return AudioFormat::Int32;
    return AudioFormat::Int16;
}

constexpr uint8_t audioFormatBytesPerSample(AudioFormat f) {
    switch (f) {
        case AudioFormat::Float32: return 4;
        case AudioFormat::Int32:   return 4;
        case AudioFormat::Int16:   return 2;
    }
    return 2;
}

} // namespace crosspad

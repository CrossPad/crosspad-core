// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file AudioFormatConvert.hpp
 * @brief Float ↔ Int16/Int32 conversion at I/O boundaries.
 *
 * Internal pipeline is float32 in [-1.0, 1.0]. Conversions only happen at
 * IAudioStream push/pull boundaries. Header-only so platforms can inline
 * tight loops; ESP-DSP SIMD path can be opted in via CROSSPAD_USE_ESP_DSP.
 *
 * Sample counts here are in *samples* (not frames). For interleaved stereo,
 * pass frames * 2 as count.
 */

#include "crosspad/audio/AudioFormat.hpp"
#include <cstdint>
#include <cstring>

#if defined(CROSSPAD_USE_ESP_DSP) && __has_include(<esp_dsp.h>)
  #include <esp_dsp.h>
  #define CROSSPAD_HAS_ESP_DSP 1
#else
  #define CROSSPAD_HAS_ESP_DSP 0
#endif

namespace crosspad {

constexpr float kInt16ScaleToFloat   = 1.0f / 32768.0f;
constexpr float kInt32ScaleToFloat   = 1.0f / 2147483648.0f;
constexpr float kFloatScaleToInt16   = 32767.0f;
constexpr float kFloatScaleToInt32   = 2147483647.0f;

inline float clampFloat(float v) {
    if (v >  1.0f) return  1.0f;
    if (v < -1.0f) return -1.0f;
    return v;
}

// ─── float → int16 ───────────────────────────────────────────────────────
inline void floatToInt16(const float* src, int16_t* dst, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        float v = clampFloat(src[i]) * kFloatScaleToInt16;
        dst[i] = static_cast<int16_t>(v);
    }
}

// ─── int16 → float ───────────────────────────────────────────────────────
inline void int16ToFloat(const int16_t* src, float* dst, uint32_t count) {
#if CROSSPAD_HAS_ESP_DSP
    // ESP-DSP has dsps_int_to_float-like helpers; fallback to scalar if unavailable.
    for (uint32_t i = 0; i < count; ++i) dst[i] = src[i] * kInt16ScaleToFloat;
#else
    for (uint32_t i = 0; i < count; ++i) dst[i] = src[i] * kInt16ScaleToFloat;
#endif
}

// ─── float → int32 ───────────────────────────────────────────────────────
inline void floatToInt32(const float* src, int32_t* dst, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        float v = clampFloat(src[i]) * kFloatScaleToInt32;
        dst[i] = static_cast<int32_t>(v);
    }
}

// ─── int32 → float ───────────────────────────────────────────────────────
inline void int32ToFloat(const int32_t* src, float* dst, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) dst[i] = src[i] * kInt32ScaleToFloat;
}

// ─── Generic dispatch ─────────────────────────────────────────────────────

/// Convert any supported format to float32 [-1.0, 1.0].
/// @param count number of *samples* (interleaved channels counted separately)
inline void convertToFloat(const void* src, AudioFormat srcFmt, float* dst, uint32_t count) {
    switch (srcFmt) {
        case AudioFormat::Float32:
            std::memcpy(dst, src, count * sizeof(float));
            break;
        case AudioFormat::Int32:
            int32ToFloat(static_cast<const int32_t*>(src), dst, count);
            break;
        case AudioFormat::Int16:
            int16ToFloat(static_cast<const int16_t*>(src), dst, count);
            break;
    }
}

/// Convert float32 [-1.0, 1.0] to any supported format.
inline void convertFromFloat(const float* src, void* dst, AudioFormat dstFmt, uint32_t count) {
    switch (dstFmt) {
        case AudioFormat::Float32:
            std::memcpy(dst, src, count * sizeof(float));
            break;
        case AudioFormat::Int32:
            floatToInt32(src, static_cast<int32_t*>(dst), count);
            break;
        case AudioFormat::Int16:
            floatToInt16(src, static_cast<int16_t*>(dst), count);
            break;
    }
}

} // namespace crosspad

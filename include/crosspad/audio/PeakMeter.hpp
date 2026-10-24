// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file PeakMeter.hpp
 * @brief Stereo peak level meter with exponential decay.
 *
 * Platform-agnostic utility for VU/peak metering. Feed raw peak values
 * (e.g., from an audio callback), read back smoothed levels for UI display.
 *
 * Usage:
 * ```cpp
 * crosspad::PeakMeter meter(230);  // decay factor 230/256 ≈ 0.90
 *
 * // In a periodic timer (e.g., 16ms / 60fps):
 * int16_t rawL, rawR;
 * audioOutput.getOutputLevel(rawL, rawR);
 * meter.update(rawL, rawR);
 *
 * int16_t smoothL = meter.left();
 * int16_t smoothR = meter.right();
 * ```
 */

#include <cstdint>

namespace crosspad {

class PeakMeter {
public:
    /**
     * @param decayFactor Decay per update cycle, as fixed-point /256.
     *                    230 → 0.90 decay, 238 → 0.93 decay.
     *                    Higher = slower decay (longer tail).
     */
    explicit PeakMeter(int decayFactor = 230)
        : decay_(decayFactor) {}

    /** Feed new raw peak values. Call once per UI update cycle. */
    void update(int16_t rawL, int16_t rawR) {
        peakL_ = static_cast<int16_t>((peakL_ * decay_) / 256);
        peakR_ = static_cast<int16_t>((peakR_ * decay_) / 256);
        if (rawL > peakL_) peakL_ = rawL;
        if (rawR > peakR_) peakR_ = rawR;
    }

    /** Float overload. Feeds normalized peaks [0.0, 1.0]; rescales to int16 internally. */
    void updateFloat(float rawL, float rawR) {
        if (rawL < 0) rawL = -rawL;
        if (rawR < 0) rawR = -rawR;
        if (rawL > 1.0f) rawL = 1.0f;
        if (rawR > 1.0f) rawR = 1.0f;
        update(static_cast<int16_t>(rawL * 32767.0f),
               static_cast<int16_t>(rawR * 32767.0f));
    }

    int16_t left()  const { return peakL_; }
    int16_t right() const { return peakR_; }

    /** Float accessors, normalized [0.0, 1.0]. */
    float leftFloat()  const { return peakL_ / 32767.0f; }
    float rightFloat() const { return peakR_ / 32767.0f; }

    void reset() { peakL_ = 0; peakR_ = 0; }

private:
    int decay_;
    int16_t peakL_ = 0;
    int16_t peakR_ = 0;
};

} // namespace crosspad

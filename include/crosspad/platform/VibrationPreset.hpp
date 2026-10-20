// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace crosspad {

/**
 * @brief Predefined vibration haptic effects
 *
 * Platform adapters translate these to the actual motor driver commands
 * (e.g. DRV2605 waveform sequences).
 */
enum class VibrationPreset : uint8_t {
    TouchPressed,
    TouchPressLost,
    TouchLongPress,
    TouchLongPressRepeat,
    TouchReleased,
    ImuShake,
    ImuTap,
    ImuDoubleTap,
    ImuFall,
};

} // namespace crosspad

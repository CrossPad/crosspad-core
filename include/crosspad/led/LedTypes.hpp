// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include "crosspad/led/RgbColor.hpp"

namespace crosspad {

/**
 * @brief Per-pad LED state tracking
 */
struct PadLedState {
    bool isPressed = false;
    bool isPlaying = false;
    uint8_t velocity = 0;
    uint8_t pressure = 0;
    uint8_t midiNote = 0;
    RgbColor color{0, 0, 0};
    // Match Pad's kit defaults (Pad.hpp: 0x20FF10 / 0xFF1020) so pads look
    // the same with and without an app palette loaded.
    RgbColor defaultColor{0x20, 0xFF, 0x10};
    RgbColor pressedColor{0xFF, 0x10, 0x20};
    RgbColor playingColor{0x32, 0x00, 0x05};
    bool hasCustomColor = false;
    bool isAnimating = false;
};

/**
 * @brief Animation modes available for the keypad
 */
enum class KeypadAnimationMode {
    NONE,
    WAVE,
    BOUNCE,
    RAINBOW,
    SPIRAL,
    RANDOM,
    CHASE,
    BREATHE,
    CUSTOM
};

/**
 * @brief Feedback modes that control how pads respond to touch
 */
enum class KeypadFeedbackMode {
    STANDARD,
    VELOCITY,
    AFTERTOUCH,
    SPECTRUM,
    APP_DEFINED
};

/**
 * @brief Parameters for configuring animations
 */
struct KeypadAnimationParams {
    uint32_t duration = 1000;
    uint8_t brightness = 255;
    RgbColor startColor{0, 0, 0};
    RgbColor endColor{255, 255, 255};
    bool repeat = false;
    uint8_t repeatCount = 0;
};

// Callback function types
typedef void (*KeypadNoteOnCallback)(uint8_t padIdx, bool isOn, uint8_t velocity);
typedef void (*KeypadAftertouchCallback)(uint8_t padIdx, uint8_t pressure);
typedef void (*KeypadAnimationCompleteCallback)(uint8_t padIdx);
typedef RgbColor (*KeypadCustomColorCallback)(uint8_t padIdx, bool isPressed, bool isPlaying);

} // namespace crosspad

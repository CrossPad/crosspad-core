// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cmath>
#include "crosspad/led/HslColor.hpp"

namespace crosspad {

/// @brief An 8-bit-per-channel RGB colour, as the pad LEDs take it.
class RgbColor {
public:
    /// Black.
    RgbColor() : R(0), G(0), B(0) {}
    /// A colour from red, green and blue, 0-255 each.
    RgbColor(uint8_t R, uint8_t G, uint8_t B) : R(R), G(G), B(B) {}
    uint8_t R;   ///< red, 0-255
    uint8_t G;   ///< green, 0-255
    uint8_t B;   ///< blue, 0-255

    /// True when all three channels match.
    bool operator==(const RgbColor& other) const {
        return R == other.R && G == other.G && B == other.B;
    }

    /// Convert from HSL: hue in degrees (0-360), saturation and lightness
    /// 0.0 - 1.0.
    RgbColor(HslColor hsl) {
        float c = (1 - std::fabs(2 * hsl.l - 1)) * hsl.s;
        float x = c * (1 - std::fabs(std::fmod(hsl.h / 60.0f, 2.0f) - 1));
        float m = hsl.l - c / 2;

        if (hsl.h < 60) {
            R = (uint8_t)((c + m) * 255);
            G = (uint8_t)((x + m) * 255);
            B = (uint8_t)(m * 255);
        } else if (hsl.h < 120) {
            R = (uint8_t)((x + m) * 255);
            G = (uint8_t)((c + m) * 255);
            B = (uint8_t)(m * 255);
        } else if (hsl.h < 180) {
            R = (uint8_t)(m * 255);
            G = (uint8_t)((c + m) * 255);
            B = (uint8_t)((x + m) * 255);
        } else if (hsl.h < 240) {
            R = (uint8_t)(m * 255);
            G = (uint8_t)((x + m) * 255);
            B = (uint8_t)((c + m) * 255);
        } else if (hsl.h < 300) {
            R = (uint8_t)((x + m) * 255);
            G = (uint8_t)(m * 255);
            B = (uint8_t)((c + m) * 255);
        } else {
            R = (uint8_t)((c + m) * 255);
            G = (uint8_t)(m * 255);
            B = (uint8_t)((x + m) * 255);
        }
    }

    /// The colour @p t of the way from @p start to @p end, per channel;
    /// @p t is clamped to 0.0 - 1.0.
    static RgbColor LinearBlend(const RgbColor& start, const RgbColor& end, float t) {
        t = std::fmax(0.0f, std::fmin(1.0f, t));
        uint8_t r = (uint8_t)(start.R + (end.R - start.R) * t);
        uint8_t g = (uint8_t)(start.G + (end.G - start.G) * t);
        uint8_t b = (uint8_t)(start.B + (end.B - start.B) * t);
        return RgbColor(r, g, b);
    }

    /// This colour scaled by @p brightness / 255 (255 = unchanged, 0 = black).
    RgbColor Dim(uint8_t brightness) const {
        float factor = brightness / 255.0f;
        return RgbColor((uint8_t)(R * factor), (uint8_t)(G * factor), (uint8_t)(B * factor));
    }
};

} // namespace crosspad

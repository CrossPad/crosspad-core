// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace crosspad {

/// @brief A colour as hue, saturation and lightness; RgbColor converts it.
class HslColor {
public:
    HslColor(float h, float s, float l) : h(h), s(s), l(l) {}
    float h;   ///< hue in degrees, 0-360
    float s;   ///< saturation, 0.0 - 1.0
    float l;   ///< lightness, 0.0 - 1.0
    bool operator==(const HslColor& other) const {
        return h == other.h && s == other.s && l == other.l;
    }
};

} // namespace crosspad

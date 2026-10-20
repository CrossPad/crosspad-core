// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include "crosspad/led/RgbColor.hpp"

namespace crosspad {

class HtmlColor {
public:
    HtmlColor(const RgbColor& rgb) : red(rgb.R), green(rgb.G), blue(rgb.B) {}
    HtmlColor(uint32_t hexValue) {
        red   = (hexValue >> 16) & 0xFF;
        green = (hexValue >> 8) & 0xFF;
        blue  = hexValue & 0xFF;
    }

    RgbColor toRgb() const { return RgbColor(red, green, blue); }
    operator RgbColor() const { return RgbColor(red, green, blue); }

private:
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

} // namespace crosspad

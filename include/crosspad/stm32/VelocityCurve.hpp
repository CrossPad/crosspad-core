// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace crosspad {

// Curve enum — mirrors STM app/inc/buttons.h `enum vel_curve`.
enum : uint8_t {
    STM32_VEL_LINEAR   = 0,
    STM32_VEL_EXP_SOFT = 1,   // convex: softens low end
    STM32_VEL_EXP_HARD = 2,   // concave: harder hit for high velocity
};
static constexpr uint8_t STM32_VEL_INHERIT = 0xFF; // per-pad sentinel = use global default

// EXACT integer mirror of STM app/src/buttons.c:vel_curve_apply().
// Maps a linear 0..127 hit value through `curve`, applies `floor`, clamps 1..127.
// Keep byte-identical to the STM source — test_velocity_curve pins it.
inline uint8_t stm32VelCurveApply(uint8_t v, uint8_t curve, uint8_t floor) {
    uint16_t out = v;
    switch (curve) {
        case STM32_VEL_EXP_HARD: // out = v^2/127
            out = (uint16_t)((uint32_t)v * v / 127U);
            break;
        case STM32_VEL_EXP_SOFT: // out = 127 - (127-v)^2/127
            out = (uint16_t)(127U - (uint32_t)(127U - v) * (127U - v) / 127U);
            break;
        case STM32_VEL_LINEAR:
        default:
            out = v;
            break;
    }
    if (out > 127U) out = 127U;
    if (out < floor) out = floor;
    if (out < 1U)    out = 1U;
    return (uint8_t)out;
}

} // namespace crosspad

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file PadInput.hpp
 * @brief A pad hit from outside the pads (board PAD_PRESS / PAD_RELEASE /
 *        PAD_PRESSURE / PAD_PAN, simulator pad_press / pad_release /
 *        pad_pressure / pad_pan), delivered once.
 */

#include <cstdint>

namespace crosspad {
class PadManager;
}

namespace crosspad::control {

enum class PadInput : uint8_t { Press, Release, Pressure, Pan };

/// Hands the input to @p pads as the STM's would arrive: @p value is the
/// velocity of a press, the pressure, or the pan as its 7-bit wire value
/// (0 left .. 64 centre .. 127 right); a release ignores it.
void applyPadInput(PadManager& pads, PadInput kind, uint8_t pad, uint8_t value);

} // namespace crosspad::control

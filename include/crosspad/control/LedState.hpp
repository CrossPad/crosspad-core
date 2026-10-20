// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file LedState.hpp
 * @brief The pad LED verb (board LED_STATE, simulator led_state), gathered once.
 *
 * Why the pads are dark: painted black, brightness zero, an animation holding
 * the strip, or coalesced frames nobody ticks out.
 */

#include <cstdint>

#include "crosspad/led/RgbColor.hpp"
#include "crosspad/pad/PadAnimator.hpp"

namespace crosspad {
class PadLedController;
}

namespace crosspad::control {

struct LedState {
    static constexpr uint8_t kPads = PadAnimator::kPadCount;

    uint8_t  brightness         = 0;   ///< what the controller scales colours by now
    bool     animating          = false;
    bool     coalesced          = false;   ///< frames go out from update() ticks only
    uint8_t  settingsBrightness = 0;   ///< the saved RGBbrightness; 0 without settings
    RgbColor colors[kPads] = {};
};

LedState readLedState(PadLedController& leds);

} // namespace crosspad::control

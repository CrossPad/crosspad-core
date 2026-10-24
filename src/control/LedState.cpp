// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/control/LedState.hpp"

#include "crosspad/pad/PadLedController.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"

namespace crosspad::control {

LedState readLedState(PadLedController& leds) {
    LedState st;
    st.brightness = leds.getBrightness();
    st.animating  = leds.getAnimating();
    st.coalesced  = leds.getCoalescedRefresh();
    if (const CrosspadSettings* cfg = CrosspadSettings::getInstance()) {
        st.settingsBrightness = cfg->RGBbrightness;
    }
    for (uint8_t i = 0; i < LedState::kPads; ++i) st.colors[i] = leds.getPadColor(i);
    return st;
}

} // namespace crosspad::control

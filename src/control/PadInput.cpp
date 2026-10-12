// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/control/PadInput.hpp"

#include "crosspad/pad/IPadLogicHandler.hpp"
#include "crosspad/pad/PadManager.hpp"

namespace crosspad::control {

void applyPadInput(PadManager& pads, PadInput kind, uint8_t pad, uint8_t value) {
    switch (kind) {
    case PadInput::Press:    pads.handlePadPress(pad, value); break;
    case PadInput::Release:  pads.handlePadRelease(pad); break;
    case PadInput::Pressure: pads.handlePadPressure(pad, value); break;
    case PadInput::Pan:      pads.notifyPadPan(pad, IPadLogicHandler::panFromWire(value)); break;
    }
}

} // namespace crosspad::control

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/midi/IMidiOutput.hpp"

namespace crosspad {

/**
 * @brief No-op IMidiOutput stub for platforms without MIDI hardware.
 *
 * Used as the default in PlatformServices until a real MIDI output
 * is registered via setMidiOutput().
 */
class NullMidiOutput : public IMidiOutput {
public:
    void sendNoteOn(uint8_t, uint8_t) override {}
    void sendNoteOff(uint8_t, uint8_t) override {}
};

} // namespace crosspad

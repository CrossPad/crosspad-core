#pragma once
#include "crosspad/midi/IMidiOutput.hpp"

namespace crosspad {

/**
 * @brief ESP-IDF MIDI output — wraps the existing DigitalPadSendNoteOn/Off functions
 */
class EspMidiOutput : public IMidiOutput {
public:
    void sendNoteOn(uint8_t note, uint8_t channel) override;
    void sendNoteOff(uint8_t note, uint8_t channel) override;
};

} // namespace crosspad

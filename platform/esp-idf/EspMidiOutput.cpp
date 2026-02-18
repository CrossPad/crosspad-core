#include "EspMidiOutput.hpp"

// These are defined in main/midiCallbacks.cpp
extern "C" void DigitalPadSendNoteOn(uint8_t note, uint8_t channel);
extern "C" void DigitalPadSendNoteOff(uint8_t note, uint8_t channel);

namespace crosspad {

void EspMidiOutput::sendNoteOn(uint8_t note, uint8_t channel) {
    DigitalPadSendNoteOn(note, channel);
}

void EspMidiOutput::sendNoteOff(uint8_t note, uint8_t channel) {
    DigitalPadSendNoteOff(note, channel);
}

} // namespace crosspad

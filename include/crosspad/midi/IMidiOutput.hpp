#pragma once
#include <cstdint>

namespace crosspad {

/**
 * @brief Abstract MIDI output interface
 *
 * Platform implementations:
 *   - ESP-IDF: wraps DigitalPadSendNoteOn/Off (TinyUSB + BLE MIDI)
 *   - Arduino: wraps MIDI library
 *   - Linux: wraps ALSA/JACK MIDI
 */
class IMidiOutput {
public:
    virtual ~IMidiOutput() = default;

    virtual void sendNoteOn(uint8_t note, uint8_t channel) = 0;
    virtual void sendNoteOff(uint8_t note, uint8_t channel) = 0;
};

/// Platform-injected singleton (same pattern as getEventBus())
IMidiOutput& getMidiOutput();

} // namespace crosspad

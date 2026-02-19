#include "EspMidiOutput.hpp"
#include "esp_log.h"

static const char* TAG = "EspMidiOutput";

namespace crosspad {

void EspMidiOutput::sendNoteOn(uint8_t note, uint8_t channel) {
    if (noteOnCb_) {
        noteOnCb_(note, channel);
    } else {
        ESP_LOGW(TAG, "sendNoteOn called but no callback set");
    }
}

void EspMidiOutput::sendNoteOff(uint8_t note, uint8_t channel) {
    if (noteOffCb_) {
        noteOffCb_(note, channel);
    } else {
        ESP_LOGW(TAG, "sendNoteOff called but no callback set");
    }
}

} // namespace crosspad

#pragma once

/**
 * @file CrosspadSettings.hpp
 * @brief Portable settings container — pure data model, no I/O
 *
 * Persistence is handled by platform-specific IKeyValueStore implementations
 * (NVS on ESP-IDF, SPIFFS on Arduino, filesystem on Linux).
 */

#include "crosspad/settings/KeypadSettings.hpp"
#include "crosspad/settings/STMRoutingSettings.hpp"
#include "crosspad/settings/ESPRoutingSettings.hpp"
#include "crosspad/settings/WirelessSettings.hpp"
#include "crosspad/settings/AudioSettings.hpp"
#include "crosspad/settings/VibrationSettings.hpp"

namespace crosspad {

static constexpr int CROSSPAD_MAX_PADS = 16;

class CrosspadSettings {
public:
    static CrosspadSettings* getInstance() {
        if (instance == nullptr) {
            instance = new CrosspadSettings();
        }
        return instance;
    }

    KeypadSettings keypad;
    STMRoutingSettings stmRouting;
    ESPRoutingSettings esp32Routing;
    WirelessSettings wireless;
    AudioSettings audio[CROSSPAD_MAX_PADS];
    AudioSettings masterFX;
    VibrationSettings vibration;
    uint8_t LCDbrightness;
    bool LCDAutoBrightness;
    uint8_t RGBbrightness;
    bool AudioEngineEnabled;
    uint8_t Kit;

private:
    static inline CrosspadSettings* instance = nullptr;

    CrosspadSettings() {
        keypad = KeypadSettings();
        stmRouting = STMRoutingSettings();
        esp32Routing = ESPRoutingSettings();
        wireless = WirelessSettings();
        vibration = VibrationSettings();
        LCDbrightness = 100;
        LCDAutoBrightness = true;
        RGBbrightness = 100;
        AudioEngineEnabled = true;
        Kit = 0;
        masterFX = AudioSettings();
        for (int i = 0; i < CROSSPAD_MAX_PADS; i++) {
            audio[i] = AudioSettings();
        }
    }
};

} // namespace crosspad

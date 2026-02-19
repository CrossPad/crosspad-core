#pragma once

/**
 * @file CrosspadSettings.hpp
 * @brief Portable settings container with IKeyValueStore-based persistence
 *
 * Field-to-key mapping is defined here so it's consistent across all platforms.
 * Platform-specific IKeyValueStore implementations provide the actual storage
 * (NVS on ESP-IDF, SPIFFS on Arduino, filesystem on Linux).
 */

#include "crosspad/settings/KeypadSettings.hpp"
#include "crosspad/settings/STMRoutingSettings.hpp"
#include "crosspad/settings/ESPRoutingSettings.hpp"
#include "crosspad/settings/WirelessSettings.hpp"
#include "crosspad/settings/AudioSettings.hpp"
#include "crosspad/settings/VibrationSettings.hpp"
#include "crosspad/settings/IKeyValueStore.hpp"

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

    /// Load all settings from a key-value store
    void loadFrom(IKeyValueStore& store);

    /// Save all settings to a key-value store
    void saveTo(IKeyValueStore& store);

    KeypadSettings keypad;
    STMRoutingSettings stmRouting;
    ESPRoutingSettings esp32Routing;
    WirelessSettings wireless;
    AudioSettings audio[CROSSPAD_MAX_PADS];
    AudioSettings previewAudio;
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
        previewAudio = AudioSettings();
        for (int i = 0; i < CROSSPAD_MAX_PADS; i++) {
            audio[i] = AudioSettings();
        }
    }
};

} // namespace crosspad

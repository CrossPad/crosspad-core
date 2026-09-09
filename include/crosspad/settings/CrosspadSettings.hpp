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
#include "crosspad/settings/AudioEngineSettings.hpp"
#include "crosspad/settings/VibrationSettings.hpp"
#include "crosspad/settings/Stm32Settings.hpp"
#include "crosspad/settings/ImuMidiSettings.hpp"
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
    AudioEngineSettings audioEngine;
    VibrationSettings vibration;
    ImuMidiSettings imuMidi;
    Stm32Settings stm;
    uint8_t LCDbrightness;
    uint8_t themeColorIndex;
    uint8_t perfStatsFlags;   // bitfield: bit0=FPS, bit1=CPU, bit2=MEM
    uint8_t RGBbrightness;
    bool quickSettingsEnabled; // status-bar drawer (header dropdown + arrow) enabled
    bool AudioEngineEnabled;
    /// Preferred USB device profile applied at boot: false = CDC+MIDI (OTA/console
    /// available), true = USB Audio (UAC2). Switching re-enumerates the device.
    bool usbAudioEnabled;
    /// What the USB host records, as an index into the platform's route preset
    /// table (0 = built-in microphones). Re-applied at boot so a device set up
    /// once keeps recording the same source. 0x7F means the routing was put
    /// somewhere no preset describes and should not be re-applied.
    uint8_t usbAudioPreset;
    uint8_t Kit;

private:
    static inline CrosspadSettings* instance = nullptr;

    CrosspadSettings() {
        keypad = KeypadSettings();
        stmRouting = STMRoutingSettings();
        esp32Routing = ESPRoutingSettings();
        wireless = WirelessSettings();
        vibration = VibrationSettings();
        imuMidi = ImuMidiSettings();
        LCDbrightness = 100;
        themeColorIndex = 0;
        perfStatsFlags = 0;
        RGBbrightness = 100;
        quickSettingsEnabled = true;
        AudioEngineEnabled = true;
        usbAudioEnabled = false;   // CDC+MIDI by default — keeps OTA/console reachable
        usbAudioPreset = 0;        // built-in microphones
        Kit = 0;
        masterFX = AudioSettings();
        previewAudio = AudioSettings();
        for (int i = 0; i < CROSSPAD_MAX_PADS; i++) {
            audio[i] = AudioSettings();
        }
    }
};

} // namespace crosspad

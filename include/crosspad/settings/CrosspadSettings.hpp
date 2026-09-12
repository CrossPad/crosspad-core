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
    /// Preferred USB device profile applied at boot: true = USB Audio (UAC2),
    /// false = CDC+MIDI. Switching re-enumerates the device. Audio is the
    /// default because that is what the instrument is for; it costs the CDC
    /// endpoint, so a board in it is reached over SysEx on USB MIDI, which
    /// exists in both profiles.
    bool usbAudioEnabled;
    /// Ask on the device before a host-originated switch to the CDC profile
    /// (SysEx 0x1B 0x01) is honoured. The CDC profile is the console, OTA and
    /// file plane, so an unknown computer should not get it unattended. The
    /// switch back to audio, the Settings dropdown and CDC USB_DEFAULT never ask.
    bool usbSerialGuard;
    /// What the USB host records, as an index into the platform's route preset
    /// table (0 = built-in microphones). Re-applied at boot so a device set up
    /// once keeps recording the same source. 0x7F means the routing was put
    /// somewhere no preset describes and should not be re-applied.
    uint8_t usbAudioPreset;
    /// Gain on the capture bus the USB mic endpoint carries, in dB above unity.
    /// The digital tap has none of the codec ADC's input gain, which is why a
    /// take is quiet next to the old analogue capture: a velocity-127 hit lands
    /// at -16.7 dBFS, so roughly 11 dB buys a -6 dBFS recording. Raising the
    /// channel instead would raise the speakers with it, so recording gets its
    /// own knob. Percent would not fit -- 343 % is past a uint8_t.
    uint8_t usbAudioGainDb;
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
        usbAudioEnabled = true;    // UAC2 by default; console/OTA need the CDC profile
        usbSerialGuard = true;     // a host must be allowed on the device before it gets CDC
        usbAudioPreset = 0;        // built-in microphones
        usbAudioGainDb = 0;        // unity; the bus is what the speakers get
        Kit = 0;
        masterFX = AudioSettings();
        previewAudio = AudioSettings();
        for (int i = 0; i < CROSSPAD_MAX_PADS; i++) {
            audio[i] = AudioSettings();
        }
    }
};

} // namespace crosspad

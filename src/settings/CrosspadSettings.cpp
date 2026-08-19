#include "crosspad/settings/CrosspadSettings.hpp"

namespace crosspad {

void CrosspadSettings::loadFrom(IKeyValueStore& store) {
    // Display
    LCDbrightness     = store.readU8("cfg_display", "brightness", 100);
    themeColorIndex   = store.readU8("cfg_display", "theme_clr", 0);
    perfStatsFlags    = store.readU8("cfg_display", "perf_stats", 0);
    quickSettingsEnabled = store.readBool("cfg_display", "drawer_en", true);

    // Audio (master)
    AudioEngineEnabled       = store.readBool("cfg_audio", "engine_en", true);
    usbAudioEnabled          = store.readBool("cfg_audio", "usb_audio", false);
    usbAudioPreset           = store.readU8("cfg_audio", "usb_preset", 0);
    {
        uint32_t sr = store.readU8("cfg_audio_eng", "sr_idx", 1); // 0=44100, 1=48000, 2=96000
        if (sr < AudioEngineSettings::kSampleRatesCount) {
            audioEngine.sampleRate = AudioEngineSettings::kSampleRates[sr];
        }
        uint32_t fcIdx = store.readU8("cfg_audio_eng", "fc_idx", 2); // 0=32 ... 5=512, default 64
        if (fcIdx < AudioEngineSettings::kFrameCountsCount) {
            audioEngine.frameCount = AudioEngineSettings::kFrameCounts[fcIdx];
        }
    }
    masterFX.mute            = store.readBool("cfg_audio", "mute", false);
    masterFX.speakersEnabled = store.readBool("cfg_audio", "spk_en", true);
    masterFX.inVolume        = store.readU8("cfg_audio", "in_vol", 50);
    masterFX.outVolume       = store.readU8("cfg_audio", "out_vol", 50);
    masterFX.delay.bypass    = store.readBool("cfg_audio", "delay_byp", true);
    masterFX.reverb.bypass   = store.readBool("cfg_audio", "reverb_byp", true);
    masterFX.distortion.bypass = store.readBool("cfg_audio", "dist_byp", true);
    masterFX.chorus.bypass   = store.readBool("cfg_audio", "chorus_byp", true);
    masterFX.flanger.bypass  = store.readBool("cfg_audio", "flangr_byp", true);
    masterFX.outVolumeCodec[0][0] = store.readU8("cfg_audio", "ovc00", masterFX.outVolumeCodec[0][0]);
    masterFX.outVolumeCodec[0][1] = store.readU8("cfg_audio", "ovc01", masterFX.outVolumeCodec[0][1]);
    masterFX.outVolumeCodec[1][0] = store.readU8("cfg_audio", "ovc10", masterFX.outVolumeCodec[1][0]);
    masterFX.outVolumeCodec[1][1] = store.readU8("cfg_audio", "ovc11", masterFX.outVolumeCodec[1][1]);
    masterFX.inVolumeCodec[0][0]  = store.readU8("cfg_audio", "ivc00", masterFX.inVolumeCodec[0][0]);
    masterFX.inVolumeCodec[0][1]  = store.readU8("cfg_audio", "ivc01", masterFX.inVolumeCodec[0][1]);
    masterFX.inVolumeCodec[1][0]  = store.readU8("cfg_audio", "ivc10", masterFX.inVolumeCodec[1][0]);
    masterFX.inVolumeCodec[1][1]  = store.readU8("cfg_audio", "ivc11", masterFX.inVolumeCodec[1][1]);

    // RGB
    RGBbrightness = store.readU8("cfg_rgb", "brightness", 100);

    // Keypad
    keypad.enableKeypad      = store.readBool("cfg_keypad", "enable", true);
    keypad.noteOffOnRelease  = store.readBool("cfg_keypad", "noteoff_rel", false);
    keypad.inactiveLights    = store.readBool("cfg_keypad", "inactive_lt", true);
    keypad.lightsOnNoteOn    = store.readBool("cfg_keypad", "lt_noteon", false);
    keypad.lightsOnNoteOff   = store.readBool("cfg_keypad", "lt_noteoff", false);
    keypad.upperRowFunctions = store.readBool("cfg_keypad", "upper_fn", false);
    keypad.ecoMode           = store.readBool("cfg_keypad", "eco", false);
    keypad.send2STM          = store.readBool("cfg_keypad", "send_stm", false);
    keypad.send2BLE          = store.readBool("cfg_keypad", "send_ble", true);
    keypad.send2USB          = store.readBool("cfg_keypad", "send_usb", true);
    keypad.sendCC            = store.readBool("cfg_keypad", "send_cc", false);

    // Vibration
    vibration.enable                = store.readBool("cfg_vibrate", "enable", true);
    vibration.enableVibrationOnTouch = store.readBool("cfg_vibrate", "on_touch", true);
    vibration.enableVibrationOnError = store.readBool("cfg_vibrate", "on_error", true);
    vibration.enableAudioToVibe     = store.readBool("cfg_vibrate", "audio_vib", true);
    vibration.inputMin              = store.readU8("cfg_vibrate", "in_min", 10);
    vibration.inputMax              = store.readU8("cfg_vibrate", "in_max", 127);
    vibration.outputMin             = store.readU8("cfg_vibrate", "out_min", 5);
    vibration.outputMax             = store.readU8("cfg_vibrate", "out_max", 200);
    vibration.debugging             = store.readBool("cfg_vibrate", "debug", false);

    // Wireless
    wireless.enableWiFi      = store.readBool("cfg_wireless", "wifi", false);
    wireless.enableBLE       = store.readBool("cfg_wireless", "ble", true);
    wireless.enableOSC       = store.readBool("cfg_wireless", "osc", false);
    wireless.enableOSCServer = store.readBool("cfg_wireless", "osc_srv", true);
    wireless.enableUDP       = store.readBool("cfg_wireless", "udp", true);
    wireless.enableTCP       = store.readBool("cfg_wireless", "tcp", true);
    wireless.enableWebServer = store.readBool("cfg_wireless", "websrv", true);
    wireless.bleMidiMode       = store.readU8("cfg_wireless", "ble_mode", 0);
    wireless.bleMidiNoteOffset = (int8_t)store.readU8("cfg_wireless", "ble_noff", 0);

    // STM Routing
    stmRouting.RoutingMatrixEnabled = store.readBool("cfg_stm_rt", "routing_en", true);
    stmRouting.ConfigButtonsAsMidi  = store.readBool("cfg_stm_rt", "btn_midi", false);
    stmRouting.Midi2MidiThru        = store.readBool("cfg_stm_rt", "m2mt", false);
    stmRouting.Midi2USB             = store.readBool("cfg_stm_rt", "m2usb", true);
    stmRouting.Midi2ESP             = store.readBool("cfg_stm_rt", "m2esp", false);
    stmRouting.USB2Midi             = store.readBool("cfg_stm_rt", "u2midi", false);
    stmRouting.USB2USBThru          = store.readBool("cfg_stm_rt", "u2ut", false);
    stmRouting.USB2ESP              = store.readBool("cfg_stm_rt", "u2esp", true);
    stmRouting.ESP2Midi             = store.readBool("cfg_stm_rt", "e2midi", true);
    stmRouting.ESP2USB              = store.readBool("cfg_stm_rt", "e2usb", true);
    stmRouting.ESP2ESPThru          = store.readBool("cfg_stm_rt", "e2et", false);
    stmRouting.Buttons2Midi         = store.readBool("cfg_stm_rt", "b2m", false);
    stmRouting.Buttons2USB          = store.readBool("cfg_stm_rt", "b2u", false);
    stmRouting.Buttons2ESP          = store.readBool("cfg_stm_rt", "b2e", true);

    // ESP Routing
    esp32Routing.BLE2Midi   = store.readBool("cfg_esp_rt", "ble2midi", true);
    esp32Routing.BLE2Audio  = store.readBool("cfg_esp_rt", "ble2audio", true);
    esp32Routing.Midi2BLE   = store.readBool("cfg_esp_rt", "midi2ble", true);
    esp32Routing.Midi2Audio = store.readBool("cfg_esp_rt", "midi2audio", true);

    // Kit
    Kit = store.readU8("cfg_system", "kit", 0);

    // STM running config
    stm.load(store);

    // The speaker amplifier has two representations: AudioSettings::speakersEnabled
    // (what the UI edits) and Stm32Settings::audioEn (what Stm32ConfigSync pushes
    // to REG_CTL_AUDIO_EN). Let the UI-facing one win so a stale audio_en=0 in NVS
    // cannot keep muting the speakers on every config push.
    stm.audioEn = masterFX.speakersEnabled ? 1 : 0;
}

void CrosspadSettings::saveTo(IKeyValueStore& store) {
    // Display
    store.saveU8("cfg_display", "brightness", LCDbrightness);
    store.saveU8("cfg_display", "theme_clr", themeColorIndex);
    store.saveU8("cfg_display", "perf_stats", perfStatsFlags);
    store.saveBool("cfg_display", "drawer_en", quickSettingsEnabled);

    // Audio (master)
    store.saveBool("cfg_audio", "engine_en", AudioEngineEnabled);
    store.saveBool("cfg_audio", "usb_audio", usbAudioEnabled);
    store.saveU8("cfg_audio", "usb_preset", usbAudioPreset);
    {
        uint8_t srIdx = 1;
        for (uint8_t i = 0; i < AudioEngineSettings::kSampleRatesCount; ++i) {
            if (AudioEngineSettings::kSampleRates[i] == audioEngine.sampleRate) { srIdx = i; break; }
        }
        store.saveU8("cfg_audio_eng", "sr_idx", srIdx);
        uint8_t fcIdx = 2;
        for (uint8_t i = 0; i < AudioEngineSettings::kFrameCountsCount; ++i) {
            if (AudioEngineSettings::kFrameCounts[i] == audioEngine.frameCount) { fcIdx = i; break; }
        }
        store.saveU8("cfg_audio_eng", "fc_idx", fcIdx);
    }
    store.saveBool("cfg_audio", "mute", masterFX.mute);
    store.saveBool("cfg_audio", "spk_en", masterFX.speakersEnabled);
    store.saveU8("cfg_audio", "in_vol", masterFX.inVolume);
    store.saveU8("cfg_audio", "out_vol", masterFX.outVolume);
    store.saveBool("cfg_audio", "delay_byp", masterFX.delay.bypass);
    store.saveBool("cfg_audio", "reverb_byp", masterFX.reverb.bypass);
    store.saveBool("cfg_audio", "dist_byp", masterFX.distortion.bypass);
    store.saveBool("cfg_audio", "chorus_byp", masterFX.chorus.bypass);
    store.saveBool("cfg_audio", "flangr_byp", masterFX.flanger.bypass);
    store.saveU8("cfg_audio", "ovc00", masterFX.outVolumeCodec[0][0]);
    store.saveU8("cfg_audio", "ovc01", masterFX.outVolumeCodec[0][1]);
    store.saveU8("cfg_audio", "ovc10", masterFX.outVolumeCodec[1][0]);
    store.saveU8("cfg_audio", "ovc11", masterFX.outVolumeCodec[1][1]);
    store.saveU8("cfg_audio", "ivc00", masterFX.inVolumeCodec[0][0]);
    store.saveU8("cfg_audio", "ivc01", masterFX.inVolumeCodec[0][1]);
    store.saveU8("cfg_audio", "ivc10", masterFX.inVolumeCodec[1][0]);
    store.saveU8("cfg_audio", "ivc11", masterFX.inVolumeCodec[1][1]);

    // RGB
    store.saveU8("cfg_rgb", "brightness", RGBbrightness);

    // Keypad
    store.saveBool("cfg_keypad", "enable", keypad.enableKeypad);
    store.saveBool("cfg_keypad", "noteoff_rel", keypad.noteOffOnRelease);
    store.saveBool("cfg_keypad", "inactive_lt", keypad.inactiveLights);
    store.saveBool("cfg_keypad", "lt_noteon", keypad.lightsOnNoteOn);
    store.saveBool("cfg_keypad", "lt_noteoff", keypad.lightsOnNoteOff);
    store.saveBool("cfg_keypad", "upper_fn", keypad.upperRowFunctions);
    store.saveBool("cfg_keypad", "eco", keypad.ecoMode);
    store.saveBool("cfg_keypad", "send_stm", keypad.send2STM);
    store.saveBool("cfg_keypad", "send_ble", keypad.send2BLE);
    store.saveBool("cfg_keypad", "send_usb", keypad.send2USB);
    store.saveBool("cfg_keypad", "send_cc", keypad.sendCC);

    // Vibration
    store.saveBool("cfg_vibrate", "enable", vibration.enable);
    store.saveBool("cfg_vibrate", "on_touch", vibration.enableVibrationOnTouch);
    store.saveBool("cfg_vibrate", "on_error", vibration.enableVibrationOnError);
    store.saveBool("cfg_vibrate", "audio_vib", vibration.enableAudioToVibe);
    store.saveU8("cfg_vibrate", "in_min", vibration.inputMin);
    store.saveU8("cfg_vibrate", "in_max", vibration.inputMax);
    store.saveU8("cfg_vibrate", "out_min", vibration.outputMin);
    store.saveU8("cfg_vibrate", "out_max", vibration.outputMax);
    store.saveBool("cfg_vibrate", "debug", vibration.debugging);

    // Wireless
    store.saveBool("cfg_wireless", "wifi", wireless.enableWiFi);
    store.saveBool("cfg_wireless", "ble", wireless.enableBLE);
    store.saveBool("cfg_wireless", "osc", wireless.enableOSC);
    store.saveBool("cfg_wireless", "osc_srv", wireless.enableOSCServer);
    store.saveBool("cfg_wireless", "udp", wireless.enableUDP);
    store.saveBool("cfg_wireless", "tcp", wireless.enableTCP);
    store.saveBool("cfg_wireless", "websrv", wireless.enableWebServer);
    store.saveU8("cfg_wireless", "ble_mode", wireless.bleMidiMode);
    store.saveU8("cfg_wireless", "ble_noff", (uint8_t)wireless.bleMidiNoteOffset);

    // STM Routing
    store.saveBool("cfg_stm_rt", "routing_en", stmRouting.RoutingMatrixEnabled);
    store.saveBool("cfg_stm_rt", "btn_midi", stmRouting.ConfigButtonsAsMidi);
    store.saveBool("cfg_stm_rt", "m2mt", stmRouting.Midi2MidiThru);
    store.saveBool("cfg_stm_rt", "m2usb", stmRouting.Midi2USB);
    store.saveBool("cfg_stm_rt", "m2esp", stmRouting.Midi2ESP);
    store.saveBool("cfg_stm_rt", "u2midi", stmRouting.USB2Midi);
    store.saveBool("cfg_stm_rt", "u2ut", stmRouting.USB2USBThru);
    store.saveBool("cfg_stm_rt", "u2esp", stmRouting.USB2ESP);
    store.saveBool("cfg_stm_rt", "e2midi", stmRouting.ESP2Midi);
    store.saveBool("cfg_stm_rt", "e2usb", stmRouting.ESP2USB);
    store.saveBool("cfg_stm_rt", "e2et", stmRouting.ESP2ESPThru);
    store.saveBool("cfg_stm_rt", "b2m", stmRouting.Buttons2Midi);
    store.saveBool("cfg_stm_rt", "b2u", stmRouting.Buttons2USB);
    store.saveBool("cfg_stm_rt", "b2e", stmRouting.Buttons2ESP);

    // ESP Routing
    store.saveBool("cfg_esp_rt", "ble2midi", esp32Routing.BLE2Midi);
    store.saveBool("cfg_esp_rt", "ble2audio", esp32Routing.BLE2Audio);
    store.saveBool("cfg_esp_rt", "midi2ble", esp32Routing.Midi2BLE);
    store.saveBool("cfg_esp_rt", "midi2audio", esp32Routing.Midi2Audio);

    // Kit
    store.saveU8("cfg_system", "kit", Kit);

    // STM running config
    stm.audioEn = masterFX.speakersEnabled ? 1 : 0;
    stm.save(store);
}

} // namespace crosspad

// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include "crosspad/settings/CrosspadSettings.hpp"

namespace crosspad {

namespace {

/// Clamp at the NVS boundary. Consumers clamp too (the STM, the styles, the
/// dropdowns), so a corrupt or foreign value never reached hardware -- but it
/// did sit in RAM, went back out in JSON backups, and could not be told apart
/// from a legitimate setting. The ranges are the ones the Settings sliders offer.
uint8_t readU8In(IKeyValueStore& store, const char* ns, const char* key,
                 uint8_t def, uint8_t lo, uint8_t hi) {
    const uint8_t v = store.readU8(ns, key, def);
    return v < lo ? lo : (v > hi ? hi : v);
}

constexpr uint8_t kLcdBrightnessMin = 5;    // Settings -> Display slider floor
constexpr uint8_t kPercentMax       = 100;  // brightness and every volume field

enum class FieldKind : uint8_t { Bool, U8 };

/// A setting that is stored as it is held: a bool, or a byte kept within
/// [lo, hi]. loadFrom() and saveTo() both walk kFields, so the two cannot
/// disagree on a key. `fallback` is what a store without the key answers, and
/// it -- not the constructor's value, which the load overwrites -- is what a
/// board that never saved its settings boots with.
struct Field {
    const char* ns;
    const char* key;
    FieldKind   kind;
    uint8_t     fallback;
    uint8_t     lo;
    uint8_t     hi;
    void*     (*at)(CrosspadSettings&);
};

// The typed pointer inside each accessor makes a row of the wrong kind a
// compile error.
#define CP_BOOL(ns, key, member, fallback) \
    {ns, key, FieldKind::Bool, fallback, 0, 1, [](CrosspadSettings& s) -> void* { bool* p = &s.member; return p; }}
#define CP_U8_IN(ns, key, member, fallback, lo, hi) \
    {ns, key, FieldKind::U8, fallback, lo, hi, [](CrosspadSettings& s) -> void* { uint8_t* p = &s.member; return p; }}
#define CP_U8(ns, key, member, fallback) CP_U8_IN(ns, key, member, fallback, 0, UINT8_MAX)

constexpr Field kFields[] = {
    // Display
    CP_U8_IN("cfg_display", "brightness", LCDbrightness, 100, kLcdBrightnessMin, kPercentMax),
    CP_U8("cfg_display", "theme_clr", themeColorIndex, 0),
    CP_U8("cfg_display", "perf_stats", perfStatsFlags, 0),
    CP_BOOL("cfg_display", "drawer_en", quickSettingsEnabled, true),

    // Recorder
    CP_BOOL("cfg_rec", "auto_trim", recorderAutoTrim, false),

    // Audio (master)
    CP_BOOL("cfg_audio", "engine_en", AudioEngineEnabled, true),
    /* What an unprovisioned board boots with. */
    CP_BOOL("cfg_audio", "usb_audio", usbAudioEnabled, true),
    CP_BOOL("cfg_audio", "usb_guard", usbSerialGuard, true),
    CP_U8("cfg_audio", "usb_preset", usbAudioPreset, 0),
    CP_U8("cfg_audio", "usb_gain_db", usbAudioGainDb, 0),
    CP_BOOL("cfg_audio", "mute", masterFX.mute, false),
    CP_BOOL("cfg_audio", "spk_en", masterFX.speakersEnabled, true),
    CP_U8_IN("cfg_audio", "in_vol", masterFX.inVolume, 50, 0, kPercentMax),
    CP_U8_IN("cfg_audio", "out_vol", masterFX.outVolume, 50, 0, kPercentMax),
    CP_BOOL("cfg_audio", "delay_byp", masterFX.delay.bypass, true),
    CP_BOOL("cfg_audio", "reverb_byp", masterFX.reverb.bypass, true),
    CP_BOOL("cfg_audio", "dist_byp", masterFX.distortion.bypass, true),
    CP_BOOL("cfg_audio", "chorus_byp", masterFX.chorus.bypass, true),
    CP_BOOL("cfg_audio", "flangr_byp", masterFX.flanger.bypass, true),

    // RGB
    CP_U8("cfg_rgb", "brightness", RGBbrightness, 100),

    // Keypad
    CP_BOOL("cfg_keypad", "enable", keypad.enableKeypad, true),
    CP_BOOL("cfg_keypad", "noteoff_rel", keypad.noteOffOnRelease, false),
    CP_BOOL("cfg_keypad", "inactive_lt", keypad.inactiveLights, true),
    CP_BOOL("cfg_keypad", "lt_noteon", keypad.lightsOnNoteOn, false),
    CP_BOOL("cfg_keypad", "lt_noteoff", keypad.lightsOnNoteOff, false),
    CP_BOOL("cfg_keypad", "upper_fn", keypad.upperRowFunctions, false),
    CP_BOOL("cfg_keypad", "eco", keypad.ecoMode, false),
    CP_BOOL("cfg_keypad", "send_stm", keypad.send2STM, false),
    CP_BOOL("cfg_keypad", "send_ble", keypad.send2BLE, true),
    CP_BOOL("cfg_keypad", "send_usb", keypad.send2USB, true),
    CP_BOOL("cfg_keypad", "send_cc", keypad.sendCC, false),

    // Vibration
    CP_BOOL("cfg_vibrate", "enable", vibration.enable, true),
    CP_BOOL("cfg_vibrate", "on_touch", vibration.enableVibrationOnTouch, true),
    CP_BOOL("cfg_vibrate", "on_error", vibration.enableVibrationOnError, true),
    CP_BOOL("cfg_vibrate", "audio_vib", vibration.enableAudioToVibe, true),
    CP_U8("cfg_vibrate", "in_min", vibration.inputMin, 10),
    CP_U8("cfg_vibrate", "in_max", vibration.inputMax, 127),
    CP_U8("cfg_vibrate", "out_min", vibration.outputMin, 5),
    CP_U8("cfg_vibrate", "out_max", vibration.outputMax, 200),
    CP_BOOL("cfg_vibrate", "debug", vibration.debugging, false),

    // Wireless. The fallbacks match WirelessSettings' own defaults: a device that
    // has never saved this section behaves the same as one that never had it.
    CP_BOOL("cfg_wireless", "wifi", wireless.enableWiFi, false),
    CP_BOOL("cfg_wireless", "ble", wireless.enableBLE, false),
    CP_BOOL("cfg_wireless", "remote", wireless.enableRemote, true),
    CP_BOOL("cfg_wireless", "osc", wireless.enableOSC, false),
    CP_BOOL("cfg_wireless", "osc_srv", wireless.enableOSCServer, true),
    CP_BOOL("cfg_wireless", "udp", wireless.enableUDP, true),
    CP_BOOL("cfg_wireless", "tcp", wireless.enableTCP, true),
    CP_BOOL("cfg_wireless", "websrv", wireless.enableWebServer, true),
    CP_U8("cfg_wireless", "ble_mode", wireless.bleMidiMode, 1),
    CP_U8("cfg_wireless", "ble_toff", wireless.bleMidiTxOffset, 0),

    // STM Routing
    CP_BOOL("cfg_stm_rt", "routing_en", stmRouting.RoutingMatrixEnabled, true),
    CP_BOOL("cfg_stm_rt", "btn_midi", stmRouting.ConfigButtonsAsMidi, false),
    CP_BOOL("cfg_stm_rt", "m2mt", stmRouting.Midi2MidiThru, false),
    CP_BOOL("cfg_stm_rt", "m2usb", stmRouting.Midi2USB, true),
    CP_BOOL("cfg_stm_rt", "m2esp", stmRouting.Midi2ESP, false),
    CP_BOOL("cfg_stm_rt", "u2midi", stmRouting.USB2Midi, false),
    CP_BOOL("cfg_stm_rt", "u2ut", stmRouting.USB2USBThru, false),
    CP_BOOL("cfg_stm_rt", "u2esp", stmRouting.USB2ESP, true),
    CP_BOOL("cfg_stm_rt", "e2midi", stmRouting.ESP2Midi, true),
    CP_BOOL("cfg_stm_rt", "e2usb", stmRouting.ESP2USB, true),
    CP_BOOL("cfg_stm_rt", "e2et", stmRouting.ESP2ESPThru, false),
    CP_BOOL("cfg_stm_rt", "b2m", stmRouting.Buttons2Midi, false),
    CP_BOOL("cfg_stm_rt", "b2u", stmRouting.Buttons2USB, false),
    CP_BOOL("cfg_stm_rt", "b2e", stmRouting.Buttons2ESP, true),

    // ESP Routing
    CP_BOOL("cfg_esp_rt", "ble2midi", esp32Routing.BLE2Midi, true),
    CP_BOOL("cfg_esp_rt", "ble2audio", esp32Routing.BLE2Audio, true),
    CP_BOOL("cfg_esp_rt", "midi2ble", esp32Routing.Midi2BLE, true),
    CP_BOOL("cfg_esp_rt", "midi2audio", esp32Routing.Midi2Audio, true),

    // Kit
    CP_U8("cfg_system", "kit", Kit, 0),
};

#undef CP_BOOL
#undef CP_U8_IN
#undef CP_U8

/// One codec output or input level: both codecs, both pairs of each.
struct CodecLevel {
    const char* key;
    uint8_t     codec;
    uint8_t     pair;
};
constexpr CodecLevel kOutCodecLevels[] = {{"ovc00", 0, 0}, {"ovc01", 0, 1}, {"ovc10", 1, 0}, {"ovc11", 1, 1}};
constexpr CodecLevel kInCodecLevels[]  = {{"ivc00", 0, 0}, {"ivc01", 0, 1}, {"ivc10", 1, 0}, {"ivc11", 1, 1}};

/// The per-slot keys of cfg_imu_midi are "s<slot>_<name>".
void imuSlotKey(char* key, size_t size, size_t slot, const char* name) {
    snprintf(key, size, "s%u_%s", (unsigned)slot, name);
}
constexpr size_t kImuKeyMax = 16;

} // namespace

void CrosspadSettings::loadFrom(IKeyValueStore& store) {
    for (const Field& f : kFields) {
        if (f.kind == FieldKind::Bool) {
            *static_cast<bool*>(f.at(*this)) = store.readBool(f.ns, f.key, f.fallback != 0);
        } else {
            *static_cast<uint8_t*>(f.at(*this)) = readU8In(store, f.ns, f.key, f.fallback, f.lo, f.hi);
        }
    }

    /* A key of its own, not "launcher": every board that ever saved its
     * settings holds a Grid or List there from when the board ignored it and
     * always opened on the pad map. Reading that would move those boards off
     * the map the day the setting started to count. */
    launcherStyle = (LauncherStyle)readU8In(store, "cfg_display", "home_view",
                                            (uint8_t)LauncherStyle::Pads,
                                            (uint8_t)LauncherStyle::Grid, (uint8_t)LauncherStyle::Pads);
    touchSensitivity = (TouchSensitivity)readU8In(store, "cfg_display", "touch_sens",
                                                  (uint8_t)TouchSensitivity::Low,
                                                  (uint8_t)TouchSensitivity::Low,
                                                  (uint8_t)TouchSensitivity::High);

    {
        uint32_t sr = store.readU8("cfg_audio_eng", "sr_idx", 0); // 0=44100, 1=48000, 2=96000
        if (sr < AudioEngineSettings::kSampleRatesCount) {
            audioEngine.sampleRate = AudioEngineSettings::kSampleRates[sr];
        }
        uint32_t fcIdx = store.readU8("cfg_audio_eng", "fc_idx", 2); // 0=32 ... 5=512, default 64
        if (fcIdx < AudioEngineSettings::kFrameCountsCount) {
            audioEngine.frameCount = AudioEngineSettings::kFrameCounts[fcIdx];
        }
        const int32_t q = store.readI32("cfg_audio_eng", "q_frames", (int32_t)audioEngine.queueFrames);
        audioEngine.queueFrames = (q >= 0 && q <= (int32_t)AudioEngineSettings::kMaxQueueFrames) ? (uint32_t)q : 0;
    }

    // A level the store does not hold stays what it is.
    for (const CodecLevel& l : kOutCodecLevels) {
        uint8_t& v = masterFX.outVolumeCodec[l.codec][l.pair];
        v = readU8In(store, "cfg_audio", l.key, v, 0, kPercentMax);
    }
    for (const CodecLevel& l : kInCodecLevels) {
        uint8_t& v = masterFX.inVolumeCodec[l.codec][l.pair];
        v = readU8In(store, "cfg_audio", l.key, v, 0, kPercentMax);
    }

    /* IMU -> MIDI. Every fallback below is the value ImuMidiSettings' constructor
     * uses; a board that never saved this section must behave like one that never
     * had it. (The enableBLE slip of 8.09 was exactly this going wrong.) The span
     * persists as an integer because the store has no float and degrees need no
     * fraction. */
    {
        const ImuMidiSettings d{};
        imuMidi.enabled       = store.readBool("cfg_imu_midi", "en", d.enabled);
        imuMidi.minIntervalMs = store.readU8("cfg_imu_midi", "ival", d.minIntervalMs);
        char key[kImuKeyMax];
        for (size_t i = 0; i < kImuMidiSlots; ++i) {
            auto &slot = imuMidi.slots[i];
            const auto &dd = d.slots[i];
            imuSlotKey(key, sizeof(key), i, "en");
            slot.enabled = store.readBool("cfg_imu_midi", key, dd.enabled);
            imuSlotKey(key, sizeof(key), i, "src");
            slot.source = store.readU8("cfg_imu_midi", key, dd.source);
            imuSlotKey(key, sizeof(key), i, "tgt");
            slot.target = store.readU8("cfg_imu_midi", key, dd.target);
            imuSlotKey(key, sizeof(key), i, "cc");
            slot.cc = store.readU8("cfg_imu_midi", key, dd.cc);
            imuSlotKey(key, sizeof(key), i, "ch");
            slot.channel = store.readU8("cfg_imu_midi", key, dd.channel);
            imuSlotKey(key, sizeof(key), i, "inv");
            slot.invert = store.readBool("cfg_imu_midi", key, dd.invert);
            imuSlotKey(key, sizeof(key), i, "span");
            slot.inputSpan = (float)store.readI32("cfg_imu_midi", key,
                                                  (int32_t)dd.inputSpan);
        }
    }

    wireless.bleMidiNoteOffset = (int8_t)store.readU8("cfg_wireless", "ble_noff", 0);

    // STM running config
    stm.load(store);

    // The speaker amplifier has two representations: AudioSettings::speakersEnabled
    // (what the UI edits) and Stm32Settings::audioEn (what Stm32ConfigSync pushes
    // to REG_CTL_AUDIO_EN). Let the UI-facing one win so a stale audio_en=0 in NVS
    // cannot keep muting the speakers on every config push.
    stm.audioEn = masterFX.speakersEnabled ? 1 : 0;
}

void CrosspadSettings::saveTo(IKeyValueStore& store) {
    for (const Field& f : kFields) {
        if (f.kind == FieldKind::Bool) {
            store.saveBool(f.ns, f.key, *static_cast<const bool*>(f.at(*this)));
        } else {
            store.saveU8(f.ns, f.key, *static_cast<const uint8_t*>(f.at(*this)));
        }
    }

    store.saveU8("cfg_display", "home_view", (uint8_t)launcherStyle);
    store.saveU8("cfg_display", "touch_sens", (uint8_t)touchSensitivity);

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
        store.saveI32("cfg_audio_eng", "q_frames", (int32_t)audioEngine.queueFrames);
    }

    for (const CodecLevel& l : kOutCodecLevels) {
        store.saveU8("cfg_audio", l.key, masterFX.outVolumeCodec[l.codec][l.pair]);
    }
    for (const CodecLevel& l : kInCodecLevels) {
        store.saveU8("cfg_audio", l.key, masterFX.inVolumeCodec[l.codec][l.pair]);
    }

    // IMU -> MIDI
    store.saveBool("cfg_imu_midi", "en", imuMidi.enabled);
    store.saveU8("cfg_imu_midi", "ival", imuMidi.minIntervalMs);
    {
        char key[kImuKeyMax];
        for (size_t i = 0; i < kImuMidiSlots; ++i) {
            const auto &slot = imuMidi.slots[i];
            imuSlotKey(key, sizeof(key), i, "en");
            store.saveBool("cfg_imu_midi", key, slot.enabled);
            imuSlotKey(key, sizeof(key), i, "src");
            store.saveU8("cfg_imu_midi", key, slot.source);
            imuSlotKey(key, sizeof(key), i, "tgt");
            store.saveU8("cfg_imu_midi", key, slot.target);
            imuSlotKey(key, sizeof(key), i, "cc");
            store.saveU8("cfg_imu_midi", key, slot.cc);
            imuSlotKey(key, sizeof(key), i, "ch");
            store.saveU8("cfg_imu_midi", key, slot.channel);
            imuSlotKey(key, sizeof(key), i, "inv");
            store.saveBool("cfg_imu_midi", key, slot.invert);
            imuSlotKey(key, sizeof(key), i, "span");
            store.saveI32("cfg_imu_midi", key, (int32_t)slot.inputSpan);
        }
    }

    store.saveU8("cfg_wireless", "ble_noff", (uint8_t)wireless.bleMidiNoteOffset);

    // STM running config
    stm.audioEn = masterFX.speakersEnabled ? 1 : 0;
    stm.save(store);
}

} // namespace crosspad

#pragma once
#include <cstdint>
#include <cstdio>
#include "crosspad/settings/IKeyValueStore.hpp"

namespace crosspad {

// ESP-authoritative mirror of the STM running config (charger + pad mapping
// excluded — see plan Global Constraints). Defaults MUST equal the STM
// compiled-in defaults so the boot push changes nothing on day 1.
struct Stm32Settings {
    static constexpr int kPads = 16;

    // system
    uint8_t audioEn       = 0;
    uint8_t autoSleep     = 1;
    uint8_t midiChannel   = 0;     // 0..15
    int8_t  noteOffset    = 0;     // signed semitone offset
    bool    keypadEnabled = true;  // -> REG_CTL_KEYPAD_DIS = !keypadEnabled
    uint8_t usbGateStart  = 1;
    uint8_t usbGateEnd    = 1;
    uint8_t en3v3Hold     = 1;

    // timeouts
    uint16_t dimTimeoutS  = 1000;
    uint16_t sleepTimeoutS = 1800;
    uint16_t longPressMs  = 2000;
    uint8_t  usbSuspendS  = 30;
    uint8_t  wakeBrightness = 50;

    // encoder -> CC
    uint8_t encCcMode = 0;  // off
    uint8_t encCcNum  = 1;
    uint8_t encCcChan = 0;
    uint8_t encCcStep = 1;

    // analog globals
    uint8_t velCurveDef = 0;   // VEL_LINEAR
    uint8_t velFloorDef = 1;
    uint8_t ccPressBase = 16;
    uint8_t panCcEn     = 0;
    uint8_t ccPanBase   = 48;
    uint8_t analogMinMs = 16;
    uint8_t analogDelta = 2;

    // per-pad config
    uint16_t padSensitivity[kPads];
    uint8_t  padLrRatio[kPads];
    uint8_t  padAnalogMode[kPads];  // enum analog_mode (2 = AFTERTOUCH)
    uint8_t  padVelCurve[kPads];    // 0xFF = inherit
    uint8_t  padVelFloor[kPads];    // 0xFF = inherit

    Stm32Settings() {
        for (int i = 0; i < kPads; ++i) {
            padSensitivity[i] = 1;
            padLrRatio[i]     = 50;
            padAnalogMode[i]  = 2;
            padVelCurve[i]    = 0xFF;
            padVelFloor[i]    = 0xFF;
        }
    }

    void load(IKeyValueStore& s) {
        audioEn       = s.readU8("cfg_stm", "audio_en", audioEn);
        autoSleep     = s.readU8("cfg_stm", "auto_sleep", autoSleep);
        midiChannel   = s.readU8("cfg_stm", "midi_chan", midiChannel);
        noteOffset    = (int8_t)s.readI32("cfg_stm", "note_off", noteOffset);
        keypadEnabled = s.readBool("cfg_stm", "keypad_en", keypadEnabled);
        usbGateStart  = s.readU8("cfg_stm", "usb_gate_s", usbGateStart);
        usbGateEnd    = s.readU8("cfg_stm", "usb_gate_e", usbGateEnd);
        en3v3Hold     = s.readU8("cfg_stm", "en_3v3", en3v3Hold);
        dimTimeoutS   = (uint16_t)s.readI32("cfg_stm", "dim_to", dimTimeoutS);
        sleepTimeoutS = (uint16_t)s.readI32("cfg_stm", "sleep_to", sleepTimeoutS);
        longPressMs   = (uint16_t)s.readI32("cfg_stm", "long_press", longPressMs);
        usbSuspendS   = s.readU8("cfg_stm", "usb_susp", usbSuspendS);
        wakeBrightness = s.readU8("cfg_stm", "wake_br", wakeBrightness);
        encCcMode = s.readU8("cfg_stm", "enc_mode", encCcMode);
        encCcNum  = s.readU8("cfg_stm", "enc_num", encCcNum);
        encCcChan = s.readU8("cfg_stm", "enc_chan", encCcChan);
        encCcStep = s.readU8("cfg_stm", "enc_step", encCcStep);
        velCurveDef = s.readU8("cfg_stm", "vcurve", velCurveDef);
        velFloorDef = s.readU8("cfg_stm", "vfloor", velFloorDef);
        ccPressBase = s.readU8("cfg_stm", "cc_press", ccPressBase);
        panCcEn     = s.readU8("cfg_stm", "pan_en", panCcEn);
        ccPanBase   = s.readU8("cfg_stm", "cc_pan", ccPanBase);
        analogMinMs = s.readU8("cfg_stm", "an_min_ms", analogMinMs);
        analogDelta = s.readU8("cfg_stm", "an_delta", analogDelta);
        for (int i = 0; i < kPads; ++i) {
            char k[12];
            snprintf(k, sizeof k, "p%d_sens", i);  padSensitivity[i] = (uint16_t)s.readI32("cfg_stm", k, padSensitivity[i]);
            snprintf(k, sizeof k, "p%d_lr", i);     padLrRatio[i]     = s.readU8("cfg_stm", k, padLrRatio[i]);
            snprintf(k, sizeof k, "p%d_an", i);     padAnalogMode[i]  = s.readU8("cfg_stm", k, padAnalogMode[i]);
            snprintf(k, sizeof k, "p%d_vc", i);     padVelCurve[i]    = s.readU8("cfg_stm", k, padVelCurve[i]);
            snprintf(k, sizeof k, "p%d_vf", i);     padVelFloor[i]    = s.readU8("cfg_stm", k, padVelFloor[i]);
        }
    }

    void save(IKeyValueStore& s) const {
        s.saveU8("cfg_stm", "audio_en", audioEn);
        s.saveU8("cfg_stm", "auto_sleep", autoSleep);
        s.saveU8("cfg_stm", "midi_chan", midiChannel);
        s.saveI32("cfg_stm", "note_off", noteOffset);
        s.saveBool("cfg_stm", "keypad_en", keypadEnabled);
        s.saveU8("cfg_stm", "usb_gate_s", usbGateStart);
        s.saveU8("cfg_stm", "usb_gate_e", usbGateEnd);
        s.saveU8("cfg_stm", "en_3v3", en3v3Hold);
        s.saveI32("cfg_stm", "dim_to", dimTimeoutS);
        s.saveI32("cfg_stm", "sleep_to", sleepTimeoutS);
        s.saveI32("cfg_stm", "long_press", longPressMs);
        s.saveU8("cfg_stm", "usb_susp", usbSuspendS);
        s.saveU8("cfg_stm", "wake_br", wakeBrightness);
        s.saveU8("cfg_stm", "enc_mode", encCcMode);
        s.saveU8("cfg_stm", "enc_num", encCcNum);
        s.saveU8("cfg_stm", "enc_chan", encCcChan);
        s.saveU8("cfg_stm", "enc_step", encCcStep);
        s.saveU8("cfg_stm", "vcurve", velCurveDef);
        s.saveU8("cfg_stm", "vfloor", velFloorDef);
        s.saveU8("cfg_stm", "cc_press", ccPressBase);
        s.saveU8("cfg_stm", "pan_en", panCcEn);
        s.saveU8("cfg_stm", "cc_pan", ccPanBase);
        s.saveU8("cfg_stm", "an_min_ms", analogMinMs);
        s.saveU8("cfg_stm", "an_delta", analogDelta);
        for (int i = 0; i < kPads; ++i) {
            char k[12];
            snprintf(k, sizeof k, "p%d_sens", i);  s.saveI32("cfg_stm", k, padSensitivity[i]);
            snprintf(k, sizeof k, "p%d_lr", i);     s.saveU8("cfg_stm", k, padLrRatio[i]);
            snprintf(k, sizeof k, "p%d_an", i);     s.saveU8("cfg_stm", k, padAnalogMode[i]);
            snprintf(k, sizeof k, "p%d_vc", i);     s.saveU8("cfg_stm", k, padVelCurve[i]);
            snprintf(k, sizeof k, "p%d_vf", i);     s.saveU8("cfg_stm", k, padVelFloor[i]);
        }
    }
};

} // namespace crosspad

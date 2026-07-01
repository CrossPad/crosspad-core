#pragma once
#include <cstdint>
#include <cstdio>
#include "crosspad/settings/IKeyValueStore.hpp"
#include "crosspad/config/crosspad_config.h"

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
    // usbGateStart / usbGateEnd removed — USB_GATE is STM charger-supervisor
    // territory; ESP pushing these forces CHG_SRC_MANUAL and breaks STM arbitration.
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
    uint8_t velCurveDef = BOARD_VEL_CURVE_DEF;
    uint8_t velFloorDef = BOARD_VEL_FLOOR_DEF;
    uint8_t ccPressBase = BOARD_CC_PRESS_BASE_DEF;
    uint8_t panCcEn     = 0;
    uint8_t ccPanBase   = BOARD_CC_PAN_BASE_DEF;
    uint8_t analogMinMs = BOARD_ANALOG_MIN_MS_DEF;
    uint8_t analogDelta = BOARD_ANALOG_DELTA_DEF;

    // per-pad config — access ONLY via the accessors below.
#if defined(PAD_CONFIG_PER_PAD)
    uint16_t padSensitivity[kPads];
    uint8_t  padLrRatio[kPads];
    uint8_t  padAnalogMode[kPads];  // enum analog_mode (2 = AFTERTOUCH)
    uint8_t  padVelCurve[kPads];    // 0xFF = inherit
    uint8_t  padVelFloor[kPads];    // 0xFF = inherit
    uint16_t& sensitivity(int i)       { return padSensitivity[i]; }
    uint16_t  sensitivity(int i) const { return padSensitivity[i]; }
    uint8_t&  lrRatio(int i)           { return padLrRatio[i]; }
    uint8_t   lrRatio(int i) const     { return padLrRatio[i]; }
    uint8_t&  analogMode(int i)        { return padAnalogMode[i]; }
    uint8_t   analogMode(int i) const  { return padAnalogMode[i]; }
    uint8_t&  velCurve(int i)          { return padVelCurve[i]; }
    uint8_t   velCurve(int i) const    { return padVelCurve[i]; }
    uint8_t&  velFloor(int i)          { return padVelFloor[i]; }
    uint8_t   velFloor(int i) const    { return padVelFloor[i]; }
#else
    uint16_t padSensitivityG;
    uint8_t  padLrRatioG;
    uint8_t  padAnalogModeG;
    uint8_t  padVelCurveG;
    uint8_t  padVelFloorG;
    uint16_t& sensitivity(int)       { return padSensitivityG; }
    uint16_t  sensitivity(int) const { return padSensitivityG; }
    uint8_t&  lrRatio(int)           { return padLrRatioG; }
    uint8_t   lrRatio(int) const     { return padLrRatioG; }
    uint8_t&  analogMode(int)        { return padAnalogModeG; }
    uint8_t   analogMode(int) const  { return padAnalogModeG; }
    uint8_t&  velCurve(int)          { return padVelCurveG; }
    uint8_t   velCurve(int) const    { return padVelCurveG; }
    uint8_t&  velFloor(int)          { return padVelFloorG; }
    uint8_t   velFloor(int) const    { return padVelFloorG; }
#endif

    Stm32Settings() {
#if defined(PAD_CONFIG_PER_PAD)
        for (int i = 0; i < kPads; ++i) {
            padSensitivity[i] = BOARD_PAD_SENS_DEF;
            padLrRatio[i]     = BOARD_PAD_LR_RATIO_DEF;
            padAnalogMode[i]  = BOARD_PAD_ANALOG_MODE_DEF;
            padVelCurve[i]    = 0xFF;
            padVelFloor[i]    = 0xFF;
        }
#else
        padSensitivityG = BOARD_PAD_SENS_DEF;
        padLrRatioG     = BOARD_PAD_LR_RATIO_DEF;
        padAnalogModeG  = BOARD_PAD_ANALOG_MODE_DEF;
        padVelCurveG    = 0xFF;
        padVelFloorG    = 0xFF;
#endif
    }

    void load(IKeyValueStore& s) {
        audioEn       = s.readU8("cfg_stm", "audio_en", audioEn);
        autoSleep     = s.readU8("cfg_stm", "auto_sleep", autoSleep);
        midiChannel   = s.readU8("cfg_stm", "midi_chan", midiChannel);
        noteOffset    = (int8_t)s.readI32("cfg_stm", "note_off", noteOffset);
        keypadEnabled = s.readBool("cfg_stm", "keypad_en", keypadEnabled);
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
#if defined(PAD_CONFIG_PER_PAD)
        for (int i = 0; i < kPads; ++i) {
            char k[12];
            snprintf(k, sizeof k, "p%d_sens", i);  padSensitivity[i] = (uint16_t)s.readI32("cfg_stm", k, padSensitivity[i]);
            snprintf(k, sizeof k, "p%d_lr", i);     padLrRatio[i]     = s.readU8("cfg_stm", k, padLrRatio[i]);
            snprintf(k, sizeof k, "p%d_an", i);     padAnalogMode[i]  = s.readU8("cfg_stm", k, padAnalogMode[i]);
            snprintf(k, sizeof k, "p%d_vc", i);     padVelCurve[i]    = s.readU8("cfg_stm", k, padVelCurve[i]);
            snprintf(k, sizeof k, "p%d_vf", i);     padVelFloor[i]    = s.readU8("cfg_stm", k, padVelFloor[i]);
        }
#else
        padSensitivityG = (uint16_t)s.readI32("cfg_stm", "p_sens", padSensitivityG);
        padLrRatioG     = s.readU8("cfg_stm", "p_lr", padLrRatioG);
        padAnalogModeG  = s.readU8("cfg_stm", "p_an", padAnalogModeG);
        padVelCurveG    = s.readU8("cfg_stm", "p_vc", padVelCurveG);
        padVelFloorG    = s.readU8("cfg_stm", "p_vf", padVelFloorG);
#endif
    }

    void save(IKeyValueStore& s) const {
        s.saveU8("cfg_stm", "audio_en", audioEn);
        s.saveU8("cfg_stm", "auto_sleep", autoSleep);
        s.saveU8("cfg_stm", "midi_chan", midiChannel);
        s.saveI32("cfg_stm", "note_off", noteOffset);
        s.saveBool("cfg_stm", "keypad_en", keypadEnabled);
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
#if defined(PAD_CONFIG_PER_PAD)
        for (int i = 0; i < kPads; ++i) {
            char k[12];
            snprintf(k, sizeof k, "p%d_sens", i);  s.saveI32("cfg_stm", k, padSensitivity[i]);
            snprintf(k, sizeof k, "p%d_lr", i);     s.saveU8("cfg_stm", k, padLrRatio[i]);
            snprintf(k, sizeof k, "p%d_an", i);     s.saveU8("cfg_stm", k, padAnalogMode[i]);
            snprintf(k, sizeof k, "p%d_vc", i);     s.saveU8("cfg_stm", k, padVelCurve[i]);
            snprintf(k, sizeof k, "p%d_vf", i);     s.saveU8("cfg_stm", k, padVelFloor[i]);
        }
#else
        s.saveI32("cfg_stm", "p_sens", padSensitivityG);
        s.saveU8("cfg_stm", "p_lr", padLrRatioG);
        s.saveU8("cfg_stm", "p_an", padAnalogModeG);
        s.saveU8("cfg_stm", "p_vc", padVelCurveG);
        s.saveU8("cfg_stm", "p_vf", padVelFloorG);
#endif
    }
};

} // namespace crosspad

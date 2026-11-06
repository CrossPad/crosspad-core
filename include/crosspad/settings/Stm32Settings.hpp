// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstdio>
#include <ArduinoJson.h>
#include "crosspad/settings/IKeyValueStore.hpp"
#include "crosspad/config/crosspad_config.h"
#include "crosspad/stm32/VelocityCurve.hpp"   // STM32_VEL_INHERIT sentinel

namespace crosspad {

// ESP-authoritative mirror of the STM running config (charger + pad mapping
// excluded — see plan Global Constraints). Defaults MUST equal the STM
// compiled-in defaults so the boot push changes nothing on day 1.
struct Stm32Settings {
    static constexpr int kPads = 16;

    // system
    // Speaker amplifier enable (REG_CTL_AUDIO_EN → power_set_amp_enable).
    // Mirrors AudioSettings::speakersEnabled — CrosspadSettings::loadFrom keeps
    // the two in sync, so the amp state has one owner (the Audio settings page).
    // Was 0, which silently muted the speakers on every push.
    uint8_t audioEn       = 1;
    uint8_t autoSleep     = 1;
    uint8_t midiChannel   = 0;     // 0..15
    int8_t  noteOffset    = 0;     // signed semitone offset
    bool    keypadEnabled = true;  // -> REG_CTL_KEYPAD_DIS = !keypadEnabled
    // usbGateStart / usbGateEnd removed — USB_GATE is STM charger-supervisor
    // territory; ESP pushing these forces CHG_SRC_MANUAL and breaks STM arbitration.
    uint8_t en3v3Hold     = 1;

    // timeouts
    // 600 s, not 1000: the settings UI offers a fixed preset list and 1000 is not
    // in it, so the dropdown fell back to "Never" while a live 1000 s timeout ran.
    uint16_t dimTimeoutS  = 600;
    uint16_t sleepTimeoutS = 1800;
    // Power-off hold. Read from "long_press2": the 4000 ms boards saved under
    // "long_press" would otherwise keep their old 4 s hold. STM 1.16 clamps it
    // to 1000..3700 ms, under its 4.2 s ship hold.
    static constexpr uint16_t kLongPressMinMs     = 1000;
    static constexpr uint16_t kLongPressMaxMs     = 3700;
    static constexpr uint16_t kLongPressDefaultMs = 2400;
    uint16_t longPressMs  = kLongPressDefaultMs;
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
    uint8_t panCcEn     = 1;   // pan reaches pad logic (onPadPan); the link-internal CC costs nothing
    uint8_t ccPanBase   = BOARD_CC_PAN_BASE_DEF;
    // Bit n: pad n's two cells are wired the other way round, so its pan
    // (left/right) is reported inverted. The STM's default binding has no
    // say in which cell is "left"; this is where the board's truth lives.
    uint16_t padPanSwap = 1u << 6;
    uint8_t analogMinMs = BOARD_ANALOG_MIN_MS_DEF;
    uint8_t analogDelta = BOARD_ANALOG_DELTA_DEF;
    // How long a pad must stay pressed before the STM sends its note-on
    // (REG_CTL_BTN_PRESS_DB_MS). It is also the velocity window: the STM
    // latches the deepest sample inside it. Deliberately not the STM's
    // compiled-in 15 ms -- that was 14-15.75 ms of every pad's latency, while a
    // hard hit peaks well inside 6. Shorter catches less of a slow, soft hit
    // (it reads quieter) and lets a 6-15 ms knock on the case through.
    // Release debounce stays the STM's.
    static constexpr uint8_t kPressDebounceMinMs = 2;
    static constexpr uint8_t kPressDebounceMaxMs = 15;
    uint8_t pressDebounceMs = BOARD_PRESS_DEBOUNCE_MS_DEF;
    // Adaptive hit window (REG_CTL_BTN_HIT_*, STM 1.12): the note-on as soon
    // as the press has peaked -- hitPeakSamples samples in a row hitPeakDrop
    // raw ADC counts shallower than the deepest -- but not before hitMinMs;
    // pressDebounceMs stays the longest. hitMode 0 is the fixed window above.
    // Defaults are the STM's compiled-in ones; an older STM ignores them.
    static constexpr uint8_t kHitPeakSamplesMax = 8;
    uint8_t hitMode        = 0;
    uint8_t hitMinMs       = 2;
    uint8_t hitPeakDrop    = 32;
    uint8_t hitPeakSamples = 2;
    // TRS MIDI OUT wiring (REG_CTL_MIDI_OUT_TYPE, STM 1.14): 0 Type A (data
    // on the tip), 1 Type B (data on the ring). An older STM ignores it.
    uint8_t midiOutType = 0;

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
        longPressMs   = sanitizeLongPressMs(s.readI32("cfg_stm", "long_press2", longPressMs));
        usbSuspendS   = s.readU8("cfg_stm", "usb_susp", usbSuspendS);
        wakeBrightness = s.readU8("cfg_stm", "wake_br", wakeBrightness);
        encCcMode = s.readU8("cfg_stm", "enc_mode", encCcMode);
        encCcNum  = s.readU8("cfg_stm", "enc_num", encCcNum);
        encCcChan = s.readU8("cfg_stm", "enc_chan", encCcChan);
        encCcStep = s.readU8("cfg_stm", "enc_step", encCcStep);
        velCurveDef = s.readU8("cfg_stm", "vcurve", velCurveDef);
        velFloorDef = s.readU8("cfg_stm", "vfloor", velFloorDef);
        ccPressBase = s.readU8("cfg_stm", "cc_press", ccPressBase);
        // Not read back: every board saved a 0 here before pan had a
        // consumer (core 1.17), and no UI ever offered the choice.
        (void)s.readU8("cfg_stm", "pan_en", panCcEn);
        padPanSwap  = (uint16_t)s.readI32("cfg_stm", "pan_swap", padPanSwap);
        ccPanBase   = s.readU8("cfg_stm", "cc_pan", ccPanBase);
        analogMinMs = s.readU8("cfg_stm", "an_min_ms", analogMinMs);
        analogDelta = s.readU8("cfg_stm", "an_delta", analogDelta);
        pressDebounceMs = s.readU8("cfg_stm", "press_db", pressDebounceMs);
        hitMode        = s.readU8("cfg_stm", "hit_mode", hitMode);
        hitMinMs       = s.readU8("cfg_stm", "hit_min_ms", hitMinMs);
        hitPeakDrop    = s.readU8("cfg_stm", "hit_drop", hitPeakDrop);
        hitPeakSamples = s.readU8("cfg_stm", "hit_samples", hitPeakSamples);
        midiOutType    = s.readU8("cfg_stm", "midi_out", midiOutType);
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
        s.saveI32("cfg_stm", "long_press2", longPressMs);
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
        s.saveI32("cfg_stm", "pan_swap", padPanSwap);
        s.saveU8("cfg_stm", "cc_pan", ccPanBase);
        s.saveU8("cfg_stm", "an_min_ms", analogMinMs);
        s.saveU8("cfg_stm", "an_delta", analogDelta);
        s.saveU8("cfg_stm", "press_db", pressDebounceMs);
        s.saveU8("cfg_stm", "hit_mode", hitMode);
        s.saveU8("cfg_stm", "hit_min_ms", hitMinMs);
        s.saveU8("cfg_stm", "hit_drop", hitPeakDrop);
        s.saveU8("cfg_stm", "hit_samples", hitPeakSamples);
        s.saveU8("cfg_stm", "midi_out", midiOutType);
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

    // ── JSON (settings files on the SD card) ────────────────────────────────
    // Kept apart from the KV-store path above because the source is different:
    // a file can be hand-edited or copied from another unit, so every numeric
    // read saturates into the field's valid range. A plain narrowing assign
    // would wrap (99999 -> 0x9F) and push a nonsense value to the STM.

    /// Pad tuning subset — split out so the standalone pad file and the "stm"
    /// section of a full settings file use one set of key names.
    void padsToJson(JsonObject& json) const {
        json["velCurveDef"] = velCurveDef;
        json["velFloorDef"] = velFloorDef;
        json["ccPressBase"] = ccPressBase;
        json["panCcEn"]     = panCcEn;
        json["ccPanBase"]   = ccPanBase;
        json["padPanSwap"]  = padPanSwap;
        json["analogMinMs"] = analogMinMs;
        json["analogDelta"] = analogDelta;
        json["pressDebounceMs"] = pressDebounceMs;
        json["hitMode"]         = hitMode;
        json["hitMinMs"]        = hitMinMs;
        json["hitPeakDrop"]     = hitPeakDrop;
        json["hitPeakSamples"]  = hitPeakSamples;
        JsonArray sens = json["padSensitivity"].to<JsonArray>();
        JsonArray lr   = json["padLrRatio"].to<JsonArray>();
        JsonArray an   = json["padAnalogMode"].to<JsonArray>();
        JsonArray vc   = json["padVelCurve"].to<JsonArray>();
        JsonArray vf   = json["padVelFloor"].to<JsonArray>();
        for (int i = 0; i < kPads; ++i) {
            sens.add(sensitivity(i));
            lr.add(lrRatio(i));
            an.add(analogMode(i));
            vc.add(velCurve(i));
            vf.add(velFloor(i));
        }
    }

    void padsFromJson(const JsonObject& json) {
        velCurveDef = (uint8_t)jsonInt(json["velCurveDef"], velCurveDef, 0, 2);
        velFloorDef = (uint8_t)jsonInt(json["velFloorDef"], velFloorDef, 0, 127);
        ccPressBase = (uint8_t)jsonInt(json["ccPressBase"], ccPressBase, 0, 127);
        panCcEn     = (uint8_t)jsonInt(json["panCcEn"], panCcEn, 0, 1);
        ccPanBase   = (uint8_t)jsonInt(json["ccPanBase"], ccPanBase, 0, 127);
        padPanSwap  = (uint16_t)jsonInt(json["padPanSwap"], padPanSwap, 0, 0xFFFF);
        analogMinMs = (uint8_t)jsonInt(json["analogMinMs"], analogMinMs, 0, 255);
        analogDelta = (uint8_t)jsonInt(json["analogDelta"], analogDelta, 0, 255);
        pressDebounceMs = (uint8_t)jsonInt(json["pressDebounceMs"], pressDebounceMs,
                                           kPressDebounceMinMs, kPressDebounceMaxMs);
        hitMode        = (uint8_t)jsonInt(json["hitMode"], hitMode, 0, 1);
        hitMinMs       = (uint8_t)jsonInt(json["hitMinMs"], hitMinMs, 0, kPressDebounceMaxMs);
        hitPeakDrop    = (uint8_t)jsonInt(json["hitPeakDrop"], hitPeakDrop, 0, 255);
        hitPeakSamples = (uint8_t)jsonInt(json["hitPeakSamples"], hitPeakSamples, 1, kHitPeakSamplesMax);

        JsonVariantConst sens = json["padSensitivity"];
        JsonVariantConst lr   = json["padLrRatio"];
        JsonVariantConst an   = json["padAnalogMode"];
        JsonVariantConst vc   = json["padVelCurve"];
        JsonVariantConst vf   = json["padVelFloor"];
        // Descending, so pad 0 is the value that sticks on GLOBAL pad-config
        // builds where all five accessors alias a single variable. On FULL
        // builds the direction makes no difference.
        for (int i = kPads - 1; i >= 0; --i) {
            sensitivity(i) = (uint16_t)jsonInt(sens[i], sensitivity(i), 1, 4095);
            lrRatio(i)     = (uint8_t)jsonInt(lr[i], lrRatio(i), 1, 99);
            analogMode(i)  = (uint8_t)jsonInt(an[i], analogMode(i), 0, 3);
            velCurve(i)    = (uint8_t)jsonInherit(vc[i], velCurve(i), 0, 2);
            velFloor(i)    = (uint8_t)jsonInherit(vf[i], velFloor(i), 0, 127);
        }
    }

    void toJson(JsonObject& json) const {
        json["audioEn"]        = audioEn;
        json["autoSleep"]      = autoSleep;
        json["midiChannel"]    = midiChannel;
        json["noteOffset"]     = noteOffset;
        json["keypadEnabled"]  = keypadEnabled;
        json["en3v3Hold"]      = en3v3Hold;
        json["dimTimeoutS"]    = dimTimeoutS;
        json["sleepTimeoutS"]  = sleepTimeoutS;
        json["longPressMs"]    = longPressMs;
        json["usbSuspendS"]    = usbSuspendS;
        json["wakeBrightness"] = wakeBrightness;
        json["encCcMode"]      = encCcMode;
        json["encCcNum"]       = encCcNum;
        json["encCcChan"]      = encCcChan;
        json["encCcStep"]      = encCcStep;
        JsonObject pads = json["pads"].to<JsonObject>();
        padsToJson(pads);
    }

    void fromJson(const JsonObject& json) {
        audioEn        = (uint8_t)jsonInt(json["audioEn"], audioEn, 0, 1);
        autoSleep      = (uint8_t)jsonInt(json["autoSleep"], autoSleep, 0, 1);
        midiChannel    = (uint8_t)jsonInt(json["midiChannel"], midiChannel, 0, 15);
        noteOffset     = (int8_t)jsonInt(json["noteOffset"], noteOffset, -127, 127);
        keypadEnabled  = json["keypadEnabled"] | keypadEnabled;
        en3v3Hold      = (uint8_t)jsonInt(json["en3v3Hold"], en3v3Hold, 0, 1);
        dimTimeoutS    = (uint16_t)jsonInt(json["dimTimeoutS"], dimTimeoutS, 0, 65535);
        sleepTimeoutS  = (uint16_t)jsonInt(json["sleepTimeoutS"], sleepTimeoutS, 0, 65535);
        longPressMs    = sanitizeLongPressMs(jsonInt(json["longPressMs"], longPressMs, 0, 65535));
        usbSuspendS    = (uint8_t)jsonInt(json["usbSuspendS"], usbSuspendS, 0, 255);
        wakeBrightness = (uint8_t)jsonInt(json["wakeBrightness"], wakeBrightness, 0, 100);
        encCcMode      = (uint8_t)jsonInt(json["encCcMode"], encCcMode, 0, 3);
        encCcNum       = (uint8_t)jsonInt(json["encCcNum"], encCcNum, 0, 127);
        encCcChan      = (uint8_t)jsonInt(json["encCcChan"], encCcChan, 0, 15);
        encCcStep      = (uint8_t)jsonInt(json["encCcStep"], encCcStep, 1, 127);
        if (json["pads"].is<JsonObject>()) {
            padsFromJson(json["pads"]);
        }
    }

    /// A power-off hold the STM can run. Above 3700 ms it comes from the old,
    /// 40 % longer ladder (v1.1.1 and the Founders firmware defaulted to 4000,
    /// and every backup they wrote carries it), so it gets the new default, as
    /// the "long_press2" key gives it in NVS. Keeping it would hold for 3.7 s
    /// while Settings shows a preset. Below 1000 ms the STM ran 1000 anyway.
    static uint16_t sanitizeLongPressMs(long long ms) {
        if (ms > kLongPressMaxMs) return kLongPressDefaultMs;
        return ms < kLongPressMinMs ? kLongPressMinMs : (uint16_t)ms;
    }

    /// Clamped read. An absent or non-numeric key leaves `cur` alone, so a file
    /// written by an older firmware keeps this field's current value.
    static long long jsonInt(JsonVariantConst v, long long cur, long long lo, long long hi) {
        if (!v.is<long long>()) return cur;
        long long n = v.as<long long>();
        return n < lo ? lo : (n > hi ? hi : n);
    }

    /// Same, plus the per-pad 0xFF "inherit the global default" sentinel, which
    /// is legal even though it sits outside the value range.
    static long long jsonInherit(JsonVariantConst v, long long cur, long long lo, long long hi) {
        if (v.is<long long>() && v.as<long long>() == STM32_VEL_INHERIT) return STM32_VEL_INHERIT;
        return jsonInt(v, cur, lo, hi);
    }
};

} // namespace crosspad

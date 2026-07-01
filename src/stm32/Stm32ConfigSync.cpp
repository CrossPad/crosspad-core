#include "crosspad/stm32/Stm32ConfigSync.hpp"
#include "crosspad/stm32/reg_map.h"

namespace crosspad {

bool stm32ShouldResync(int lastBootCount, uint8_t current) {
    return lastBootCount < 0 || (uint8_t)lastBootCount != current;
}

bool Stm32ConfigSync::pushAll(const Stm32Settings& s) {
    bool ok = true;
    auto w8 = [&](uint16_t reg, uint8_t v) { ok = bus_.writeReg8(reg, v) && ok; };
    // u16 registers: write HI first to stage, then LO to commit.
    // Two separate single-byte writes so that lastByteAt(HI) and lastByteAt(LO)
    // both resolve correctly in the mock and on real hardware.
    auto w16 = [&](uint16_t hi, uint16_t v) {
        w8(hi,       (uint8_t)(v >> 8));
        w8((uint16_t)(hi + 1), (uint8_t)(v & 0xFF));
    };

    // system (0x0300..0x0311)
    w8(REG_CTL_AUDIO_EN, s.audioEn);
    w8(REG_CTL_AUTO_SLEEP, s.autoSleep);
    w8(REG_CTL_MIDI_CHAN, s.midiChannel);
    w8(REG_CTL_NOTE_OFFSET, (uint8_t)s.noteOffset);
    w8(REG_CTL_KEYPAD_DIS, s.keypadEnabled ? 0 : 1);
    // USB_GATE (REG_CTL_USB_GATE_S/E) intentionally omitted — charger source
    // supervisor is STM-owned; ESP writing these registers forces CHG_SRC_MANUAL
    // and disables STM charger arbitration.
    w8(REG_CTL_EN_3V3_HOLD, s.en3v3Hold);
    w8(REG_CTL_WAKE_BRIGHT, s.wakeBrightness);
    w8(REG_CTL_USB_SUSP_S, s.usbSuspendS);

    // timeouts (u16 BE, commit on LO)
    w16(REG_CTL_DIM_TO_S_HI, s.dimTimeoutS);
    w16(REG_CTL_SLEEP_TO_S_HI, s.sleepTimeoutS);
    w16(REG_CTL_LONG_PRESS_HI, s.longPressMs);

    // encoder CC
    w8(REG_CTL_ENC_CC_MODE, s.encCcMode);
    w8(REG_CTL_ENC_CC_NUM, s.encCcNum);
    w8(REG_CTL_ENC_CC_CHAN, s.encCcChan);
    w8(REG_CTL_ENC_CC_STEP, s.encCcStep);

    // Charger registers (REG_CTL_CHG_ICHG_HI = 0x0320 .. REG_CTL_VEL_CURVE_DEF-1)
    // are intentionally skipped — charger is not ESP-authoritative.

    // analog globals (0x033E..0x0344)
    w8(REG_CTL_VEL_CURVE_DEF, s.velCurveDef);
    w8(REG_CTL_VEL_FLOOR_DEF, s.velFloorDef);
    w8(REG_CTL_CC_PRESS_BASE, s.ccPressBase);
    w8(REG_CTL_PAN_CC_EN, s.panCcEn);
    w8(REG_CTL_CC_PAN_BASE, s.ccPanBase);
    w8(REG_CTL_ANALOG_MIN_MS, s.analogMinMs);
    w8(REG_CTL_ANALOG_DELTA, s.analogDelta);

    // per-pad config (REG_PAD_CFG_BASE = 0x0800): 6-byte stride, one write per pad
    for (int i = 0; i < Stm32Settings::kPads; ++i) {
        uint8_t buf[REG_PAD_CFG_STRIDE];
        buf[REG_PAD_CFG_FIELD_SENS_HI]   = (uint8_t)(s.sensitivity(i) >> 8);
        buf[REG_PAD_CFG_FIELD_SENS_LO]   = (uint8_t)(s.sensitivity(i) & 0xFF);
        buf[REG_PAD_CFG_FIELD_LR_RATIO]  = s.lrRatio(i);
        buf[REG_PAD_CFG_FIELD_ANALOG]    = s.analogMode(i);
        buf[REG_PAD_CFG_FIELD_VEL_CURVE] = s.velCurve(i);
        buf[REG_PAD_CFG_FIELD_VEL_FLOOR] = s.velFloor(i);
        ok = bus_.writeReg((uint16_t)(REG_PAD_CFG_BASE + i * REG_PAD_CFG_STRIDE),
                           buf, REG_PAD_CFG_STRIDE) && ok;
    }
    return ok;
}

bool Stm32ConfigSync::writePadConfig(uint8_t padIdx, const Stm32Settings& s) {
    if (padIdx >= Stm32Settings::kPads) return false;
    uint8_t buf[REG_PAD_CFG_STRIDE];
    buf[REG_PAD_CFG_FIELD_SENS_HI]   = (uint8_t)(s.sensitivity(padIdx) >> 8);
    buf[REG_PAD_CFG_FIELD_SENS_LO]   = (uint8_t)(s.sensitivity(padIdx) & 0xFF);
    buf[REG_PAD_CFG_FIELD_LR_RATIO]  = s.lrRatio(padIdx);
    buf[REG_PAD_CFG_FIELD_ANALOG]    = s.analogMode(padIdx);
    buf[REG_PAD_CFG_FIELD_VEL_CURVE] = s.velCurve(padIdx);
    buf[REG_PAD_CFG_FIELD_VEL_FLOOR] = s.velFloor(padIdx);
    return bus_.writeReg((uint16_t)(REG_PAD_CFG_BASE + padIdx * REG_PAD_CFG_STRIDE),
                         buf, REG_PAD_CFG_STRIDE);
}

bool Stm32ConfigSync::readBootCount(uint8_t& out) {
    return bus_.readReg8(REG_IDENT_BOOT_COUNT, &out);
}

bool Stm32ConfigSync::readPadVelRaw(uint8_t padIdx, uint8_t& out) {
    if (padIdx >= Stm32Settings::kPads) return false;
    return bus_.readReg8((uint16_t)(REG_IN_PAD_VELRAW_BASE + padIdx), &out);
}

bool Stm32ConfigSync::readPadLive(uint8_t padIdx, Stm32PadLive& out) {
    if (padIdx >= Stm32Settings::kPads) return false;
    uint8_t sum = 0, pan = 0, vr = 0, ab[3] = {0,0,0};
    bool ok = bus_.readReg8(REG_IN_PAD_SUM_BASE    + padIdx, &sum)
           && bus_.readReg8(REG_IN_PAD_DIFF_BASE   + padIdx, &pan)
           && bus_.readReg8(REG_IN_PAD_VELRAW_BASE + padIdx, &vr)
           && bus_.readReg((uint16_t)(REG_IN_PAD_AB_BASE + padIdx * 3), ab, 3);
    if (!ok) return false;
    out.sum    = sum;
    out.pan    = (int8_t)pan;
    out.velRaw = vr;
    out.rawA   = (uint16_t)((ab[0] << 4) | (ab[1] >> 4));
    out.rawB   = (uint16_t)(((ab[1] & 0x0F) << 8) | ab[2]);
    return true;
}

bool Stm32ConfigSync::writeReg8(uint16_t reg, uint8_t v) {
    return bus_.writeReg8(reg, v);
}

bool Stm32ConfigSync::writeReg16(uint16_t hiReg, uint16_t v) {
    bool ok = bus_.writeReg8(hiReg, (uint8_t)(v >> 8));
    ok = bus_.writeReg8((uint16_t)(hiReg + 1), (uint8_t)(v & 0xFF)) && ok;
    return ok;
}

} // namespace crosspad

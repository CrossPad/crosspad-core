// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/stm32/Stm32ConfigSync.hpp"
#include "crosspad/stm32/reg_map.h"

namespace crosspad {

namespace {

// The 16-bit STM settings whose bytes the STM puts into the value it runs on
// as each one arrives. Every other 16-bit register stages its HI byte and takes
// the pair on the LO write (reg_map.h says so where it defines them).
bool takenByteByByte(uint16_t hiReg) {
    return hiReg == REG_CTL_DIM_TO_S_HI || hiReg == REG_CTL_SLEEP_TO_S_HI ||
           hiReg == REG_CTL_LONG_PRESS_HI;
}

// A 16-bit STM setting is two one-byte registers. A register that takes the
// pair on its LO write gets HI then LO. One taken byte by byte has no commit:
// written HI then LO, 600 s -> Never (0x0258 -> 0x0000) stands at 0x0058 = 88 s
// in between, and a board idle for longer goes to sleep before the LO byte
// lands. There the high byte goes to 0xFF first, so every value on the way is
// at least 0xFF00, never shorter than the old or the new one, whatever the old
// one was. A failed write ends it: the register is then as it was or at
// 0xFF00 and above, where going on would put a new byte beside an old one.
bool writeU16(IStm32RegisterBus& bus, uint16_t hiReg, uint16_t v) {
    const uint16_t loReg = (uint16_t)(hiReg + 1);
    const uint8_t  hi = (uint8_t)(v >> 8), lo = (uint8_t)(v & 0xFF);
    if (!takenByteByByte(hiReg)) {
        bool ok = bus.writeReg8(hiReg, hi);
        return bus.writeReg8(loReg, lo) && ok;
    }
    return bus.writeReg8(hiReg, 0xFF) && bus.writeReg8(loReg, lo) && bus.writeReg8(hiReg, hi);
}

} // namespace

bool stm32ShouldResync(int lastBootCount, uint8_t current) {
    return lastBootCount < 0 || (uint8_t)lastBootCount != current;
}

bool Stm32ConfigSync::pushAll(const Stm32Settings& s) {
    bool ok = true;
    auto w8 = [&](uint16_t reg, uint8_t v) { ok = bus_.writeReg8(reg, v) && ok; };
    auto w16 = [&](uint16_t hi, uint16_t v) { ok = writeU16(bus_, hi, v) && ok; };

    // system (0x0300..0x0311)
    w8(REG_CTL_AUDIO_EN, s.audioEn);
    w8(REG_CTL_AUTO_SLEEP, s.autoSleep);
    w8(REG_CTL_MIDI_CHAN, s.midiChannel);
    w8(REG_CTL_NOTE_OFFSET, (uint8_t)s.noteOffset);
    w8(REG_CTL_KEYPAD_DIS, s.keypadEnabled ? 0 : 1);
    // USB_GATE (REG_CTL_USB_GATE_S/E) intentionally omitted — charger source
    // supervisor is STM-owned; ESP writing these registers forces CHG_SRC_MANUAL
    // and disables STM charger arbitration.
    // EN_3V3_HOLD intentionally omitted too: the STM holds the ESP rail high on
    // purpose (BOOT0 race on wake, esptool bridge) and a write of 1 released it
    // to HiZ on every push; a stored 0 would cut the ESP's own power.
    w8(REG_CTL_WAKE_BRIGHT, s.wakeBrightness);
    w8(REG_CTL_USB_SUSP_S, s.usbSuspendS);

    // timeouts and hold time (u16 BE)
    w16(REG_CTL_DIM_TO_S_HI, s.dimTimeoutS);
    w16(REG_CTL_SLEEP_TO_S_HI, s.sleepTimeoutS);
    w16(REG_CTL_LONG_PRESS_HI, s.longPressMs);

    // encoder CC
    w8(REG_CTL_ENC_CC_MODE, s.encCcMode);
    w8(REG_CTL_ENC_CC_NUM, s.encCcNum);
    w8(REG_CTL_ENC_CC_CHAN, s.encCcChan);
    w8(REG_CTL_ENC_CC_STEP, s.encCcStep);

    // Charger registers (REG_CTL_CHG_ICHG_HI = 0x0320 .. REG_CTL_CHG_AUTO_LIM_LO)
    // are intentionally skipped — charger is not ESP-authoritative.

    // Press debounce (0x033C), the one button global the ESP owns: the note-on
    // waits it out. Threshold, hysteresis and release debounce stay the STM's.
    w8(REG_CTL_BTN_PRESS_DB_MS, s.pressDebounceMs);
    // Hit window (0x034E..0x0351, STM 1.12; an older STM ignores the writes).
    w8(REG_CTL_BTN_HIT_MODE, s.hitMode);
    w8(REG_CTL_BTN_HIT_MIN_MS, s.hitMinMs);
    w8(REG_CTL_BTN_HIT_PEAK_DROP, s.hitPeakDrop);
    w8(REG_CTL_BTN_HIT_PEAK_SAMPLES, s.hitPeakSamples);
    // TRS MIDI OUT type (0x0355, STM 1.14; an older STM ignores the write).
    w8(REG_CTL_MIDI_OUT_TYPE, s.midiOutType);

    // analog globals (0x033E..0x0344)
    w8(REG_CTL_VEL_CURVE_DEF, s.velCurveDef);
    w8(REG_CTL_VEL_FLOOR_DEF, s.velFloorDef);
    w8(REG_CTL_CC_PRESS_BASE, s.ccPressBase);
    w8(REG_CTL_PAN_CC_EN, s.panCcEn);
    w8(REG_CTL_CC_PAN_BASE, s.ccPanBase);
    w8(REG_CTL_ANALOG_MIN_MS, s.analogMinMs);
    w8(REG_CTL_ANALOG_DELTA, s.analogDelta);

    // pad mapping: only the swap field — the cell binding itself stays the
    // STM's; which cell is "left" is a board fact kept here.
    for (int i = 0; i < Stm32Settings::kPads; ++i) {
        w8((uint16_t)(REG_MAPPING_BASE + i * REG_MAPPING_STRIDE + REG_MAPPING_FIELD_SWAP),
           (s.padPanSwap >> i) & 1u);
    }

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

bool Stm32ConfigSync::readReg8(uint16_t reg, uint8_t& out) {
    return bus_.readReg8(reg, &out);
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
    return writeU16(bus_, hiReg, v);
}

} // namespace crosspad

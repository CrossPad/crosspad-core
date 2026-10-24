// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include "crosspad/stm32/IStm32RegisterBus.hpp"
#include "crosspad/settings/Stm32Settings.hpp"

namespace crosspad {

// true if the ESP must (re)push the full config: never synced (last < 0) or
// the STM boot count changed (STM reset since last sync).
bool stm32ShouldResync(int lastBootCount, uint8_t current);

struct Stm32PadLive {
    uint8_t  sum    = 0;   // 0..127 pressure (continuous while pressed)
    int8_t   pan    = 0;   // -127..+127 L/R
    uint8_t  velRaw = 0;   // last note-on pre-curve linear 0..127
    uint16_t rawA   = 0;   // 0..4095 raw ADC cell A
    uint16_t rawB   = 0;   // 0..4095 raw ADC cell B
};

// Pushes the ESP-authoritative STM config over the register bus. Charger and
// pad-mapping registers are intentionally excluded (see plan constraints).
class Stm32ConfigSync {
public:
    explicit Stm32ConfigSync(IStm32RegisterBus& bus) : bus_(bus) {}
    bool pushAll(const Stm32Settings& s);
    bool writePadConfig(uint8_t padIdx, const Stm32Settings& s);
    bool readBootCount(uint8_t& out);
    /// Generic single-register read — used by the Info page to report the STM
    /// firmware/protocol identity without teaching this class every register.
    bool readReg8(uint16_t reg, uint8_t& out);
    bool readPadVelRaw(uint8_t padIdx, uint8_t& out);
    bool readPadLive(uint8_t padIdx, Stm32PadLive& out);
    bool writeReg8(uint16_t reg, uint8_t v);
    bool writeReg16(uint16_t hiReg, uint16_t v);
private:
    IStm32RegisterBus& bus_;
};

} // namespace crosspad

#pragma once
#include <cstdint>
#include "crosspad/stm32/IStm32RegisterBus.hpp"
#include "crosspad/settings/Stm32Settings.hpp"

namespace crosspad {

// true if the ESP must (re)push the full config: never synced (last < 0) or
// the STM boot count changed (STM reset since last sync).
bool stm32ShouldResync(int lastBootCount, uint8_t current);

// Pushes the ESP-authoritative STM config over the register bus. Charger and
// pad-mapping registers are intentionally excluded (see plan constraints).
class Stm32ConfigSync {
public:
    explicit Stm32ConfigSync(IStm32RegisterBus& bus) : bus_(bus) {}
    bool pushAll(const Stm32Settings& s);
    bool readBootCount(uint8_t& out);
private:
    IStm32RegisterBus& bus_;
};

} // namespace crosspad

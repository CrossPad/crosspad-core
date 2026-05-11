#pragma once

#include "crosspad/protocol/Stm32CommandSender.hpp"

namespace crosspad {

class IStm32RegisterBus;

/**
 * @brief I2C-register command sender (PCB rev2 / Arduino V2).
 *
 * Each high-level command becomes a register write per
 * crosspad/stm32/reg_map.h. The transport (UART) is no longer
 * involved — control plane and the STM→ESP MIDI pipe are separate.
 */
class I2cStm32CommandSender : public Stm32CommandSender {
public:
    explicit I2cStm32CommandSender(IStm32RegisterBus& bus)
        : bus_(bus) {}

    bool sendPowerOff() override;
    bool sendBrightness(uint8_t brightness) override;
    bool sendKeepAlive() override;
    bool sendDfuRequest(bool stm32Dfu) override;
    bool sendRoutingMatrix(const STMRoutingSettings& routing) override;

private:
    IStm32RegisterBus& bus_;
};

} // namespace crosspad

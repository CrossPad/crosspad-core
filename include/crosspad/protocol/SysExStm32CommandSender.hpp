// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/protocol/Stm32CommandSender.hpp"
#include "crosspad/protocol/SysExProtocol.h"

namespace crosspad {

class IStm32Transport;

/**
 * @brief SysEx-over-UART command sender (legacy path, PCB rev1 / Arduino V1).
 *
 * Builds CrossPad-format SysEx packets (manufacturer 0x7D) and pushes
 * them through an IStm32Transport.
 */
class SysExStm32CommandSender : public Stm32CommandSender {
public:
    explicit SysExStm32CommandSender(IStm32Transport& transport)
        : transport_(transport) {}

    bool sendPowerOff() override;
    bool sendBrightness(uint8_t brightness) override;
    bool sendKeepAlive() override;
    bool sendDfuRequest(bool stm32Dfu) override;
    bool sendRoutingMatrix(const STMRoutingSettings& routing) override;

private:
    IStm32Transport& transport_;
};

} // namespace crosspad

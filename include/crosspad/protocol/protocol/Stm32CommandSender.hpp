#pragma once

#include <cstdint>
#include <cstddef>
#include "crosspad/protocol/SysExProtocol.h"

namespace crosspad {

class IStm32Transport;
class STMRoutingSettings;

/**
 * @brief Portable command builder for STM32 co-processor
 *
 * Formats SysEx packets and sends them via IStm32Transport.
 * No platform dependencies — just byte-level protocol logic.
 */
class Stm32CommandSender {
public:
    explicit Stm32CommandSender(IStm32Transport& transport) : transport_(transport) {}

    bool sendPowerOff();
    bool sendBrightness(uint8_t brightness);
    bool sendKeepAlive();
    bool sendDfuRequest(bool stm32Dfu);
    bool sendRoutingMatrix(const STMRoutingSettings& routing);

    /// Send raw MIDI data (wrapped in SysEx or pass-through)
    bool sendMidiData(const uint8_t* data, size_t len);

private:
    IStm32Transport& transport_;
};

} // namespace crosspad

#pragma once

#include <cstdint>
#include <cstddef>

namespace crosspad {

class STMRoutingSettings;

/**
 * @brief Abstract command sender for the STM32 co-processor.
 *
 * Two concrete implementations:
 *  - SysExStm32CommandSender — frames a MIDI SysEx message and sends it
 *    over an IStm32Transport (UART). Used on PCB rev1 and Arduino V1.
 *  - I2cStm32CommandSender   — writes the equivalent register on the
 *    STM32 I2C slave (see crosspad/stm32/reg_map.h). Used on PCB rev2.
 *
 * Callers use the abstract reference returned by
 * getStm32CommandSender(); they don't need to know which transport
 * carries the command.
 */
class Stm32CommandSender {
public:
    virtual ~Stm32CommandSender() = default;

    virtual bool sendPowerOff() = 0;
    virtual bool sendBrightness(uint8_t brightness) = 0;
    virtual bool sendKeepAlive() = 0;
    virtual bool sendDfuRequest(bool stm32Dfu) = 0;
    virtual bool sendRoutingMatrix(const STMRoutingSettings& routing) = 0;
};

} // namespace crosspad

#include "crosspad/protocol/SysExStm32CommandSender.hpp"
#include "crosspad/protocol/I2cStm32CommandSender.hpp"
#include "crosspad/protocol/IStm32Transport.hpp"
#include "crosspad/stm32/IStm32RegisterBus.hpp"
#include "crosspad/stm32/reg_map.h"
#include "crosspad/settings/STMRoutingSettings.hpp"

namespace crosspad {

// ----- SysEx (UART) backend -------------------------------------------------

bool SysExStm32CommandSender::sendPowerOff() {
    const uint8_t msg[] = {
        CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER,
        CROSSPAD_CMD_POWEROFF,
        CROSSPAD_SYSEX_END
    };
    return transport_.send(msg, sizeof(msg));
}

bool SysExStm32CommandSender::sendBrightness(uint8_t brightness) {
    const uint8_t msg[] = {
        CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER,
        CROSSPAD_CMD_BRIGHTNESS, brightness,
        CROSSPAD_SYSEX_END
    };
    return transport_.send(msg, sizeof(msg));
}

bool SysExStm32CommandSender::sendKeepAlive() {
    const uint8_t msg[] = {
        CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER,
        CROSSPAD_CMD_KEEP_ALIVE,
        CROSSPAD_SYSEX_END
    };
    return transport_.send(msg, sizeof(msg));
}

bool SysExStm32CommandSender::sendDfuRequest(bool stm32Dfu) {
    const uint8_t msg[] = {
        CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER,
        CROSSPAD_CMD_DFU_REQUEST, static_cast<uint8_t>(stm32Dfu ? 1 : 0),
        CROSSPAD_SYSEX_END
    };
    return transport_.send(msg, sizeof(msg));
}

bool SysExStm32CommandSender::sendRoutingMatrix(const STMRoutingSettings& routing) {
    const uint8_t msg[] = {
        CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER,
        CROSSPAD_CMD_ROUTING_MATRIX, routing.packByte1(), routing.packByte2(),
        CROSSPAD_SYSEX_END
    };
    return transport_.send(msg, sizeof(msg));
}

// ----- I2C register backend -------------------------------------------------

bool I2cStm32CommandSender::sendPowerOff() {
    return bus_.writeReg8(REG_CMD_POWEROFF, 1);
}

bool I2cStm32CommandSender::sendBrightness(uint8_t brightness) {
    if (brightness > 100) brightness = 100;
    return bus_.writeReg8(REG_CTL_BRIGHTNESS, brightness);
}

bool I2cStm32CommandSender::sendKeepAlive() {
    return bus_.writeReg8(REG_CMD_KEEPALIVE, 1);
}

bool I2cStm32CommandSender::sendDfuRequest(bool stm32Dfu) {
    // Same trigger reg on STM side; payload distinguishes target (1=STM DFU,
    // 0=request ESP reflash via STM helper, matching SysEx semantics).
    return bus_.writeReg8(REG_CMD_DFU, stm32Dfu ? 1u : 0u);
}

bool I2cStm32CommandSender::sendRoutingMatrix(const STMRoutingSettings& routing) {
    const uint8_t bytes[2] = { routing.packByte1(), routing.packByte2() };
    return bus_.writeReg(REG_CTL_ROUTING_0, bytes, sizeof(bytes));
}

} // namespace crosspad

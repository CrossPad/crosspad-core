#include "crosspad/protocol/Stm32CommandSender.hpp"
#include "crosspad/protocol/IStm32Transport.hpp"
#include "crosspad/settings/STMRoutingSettings.hpp"

namespace crosspad {

bool Stm32CommandSender::sendPowerOff() {
    const uint8_t msg[] = {
        CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER,
        CROSSPAD_CMD_POWEROFF,
        CROSSPAD_SYSEX_END
    };
    return transport_.send(msg, sizeof(msg));
}

bool Stm32CommandSender::sendBrightness(uint8_t brightness) {
    const uint8_t msg[] = {
        CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER,
        CROSSPAD_CMD_BRIGHTNESS, brightness,
        CROSSPAD_SYSEX_END
    };
    return transport_.send(msg, sizeof(msg));
}

bool Stm32CommandSender::sendKeepAlive() {
    const uint8_t msg[] = {
        CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER,
        CROSSPAD_CMD_KEEP_ALIVE,
        CROSSPAD_SYSEX_END
    };
    return transport_.send(msg, sizeof(msg));
}

bool Stm32CommandSender::sendDfuRequest(bool stm32Dfu) {
    const uint8_t msg[] = {
        CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER,
        CROSSPAD_CMD_DFU_REQUEST, static_cast<uint8_t>(stm32Dfu ? 1 : 0),
        CROSSPAD_SYSEX_END
    };
    return transport_.send(msg, sizeof(msg));
}

bool Stm32CommandSender::sendRoutingMatrix(const STMRoutingSettings& routing) {
    const uint8_t msg[] = {
        CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER,
        CROSSPAD_CMD_ROUTING_MATRIX, routing.packByte1(), routing.packByte2(),
        CROSSPAD_SYSEX_END
    };
    return transport_.send(msg, sizeof(msg));
}

bool Stm32CommandSender::sendMidiData(const uint8_t* data, size_t len) {
    if (!data || len == 0) return false;
    return transport_.send(data, len);
}

} // namespace crosspad

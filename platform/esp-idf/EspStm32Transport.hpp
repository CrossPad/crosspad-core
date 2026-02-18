#pragma once

#include "crosspad/protocol/IStm32Transport.hpp"
#include "driver/uart.h"

namespace crosspad {

/**
 * @brief ESP-IDF UART transport for STM32 communication
 */
class EspStm32Transport : public IStm32Transport {
public:
    explicit EspStm32Transport(uart_port_t port) : port_(port) {}

    bool send(const uint8_t* data, size_t len) override {
        if (!data || len == 0) return false;
        int written = uart_write_bytes(port_, data, len);
        return written == static_cast<int>(len);
    }

private:
    uart_port_t port_;
};

} // namespace crosspad

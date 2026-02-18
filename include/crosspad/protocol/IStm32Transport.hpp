#pragma once

#include <cstdint>
#include <cstddef>

namespace crosspad {

/**
 * @brief Byte-level transport interface to/from STM32 co-processor
 *
 * Platform provides the implementation (UART, SPI, BLE, etc.).
 * Used by SysExFramer (RX side) and Stm32CommandSender (TX side).
 */
class IStm32Transport {
public:
    virtual ~IStm32Transport() = default;

    /**
     * @brief Send raw bytes to STM32
     * @param data Pointer to byte buffer
     * @param len Number of bytes
     * @return true on success
     */
    virtual bool send(const uint8_t* data, size_t len) = 0;
};

} // namespace crosspad

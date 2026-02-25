// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <cstddef>

namespace crosspad {

/**
 * @brief Minimal I2C device interface for a single slave at a fixed address.
 *
 * Platform implementations bind the bus handle + slave address at construction
 * time. The consumer (e.g. Drv2605Driver) only sees raw read/write operations.
 *
 * Platform adapters:
 *   - Arduino: wraps I2CDriver (legacy ESP-IDF I2C API)
 *   - ESP-IDF: wraps i2c_master device handle
 *   - PC: not needed (no I2C hardware)
 */
class II2cDevice {
public:
    virtual ~II2cDevice() = default;

    /// Write raw bytes to the device (address is implicit)
    virtual bool write(const uint8_t* data, size_t len) = 0;

    /// Write a register address, then read @p len bytes into @p data
    virtual bool readRegister(uint8_t reg, uint8_t* data, size_t len) = 0;
};

} // namespace crosspad

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstddef>

namespace crosspad {

/**
 * @brief Portable register-level I2C bus to the STM32 co-processor.
 *
 * Platform adapters (ESP-IDF, Arduino Wire) implement this. The STM32
 * exposes a 16-bit register address space — see crosspad/stm32/reg_map.h.
 *
 * Both methods return true on success, false on bus error / NACK.
 * Implementations must serialize concurrent calls (the same bus may carry
 * codecs / touch / IMU traffic).
 */
class IStm32RegisterBus {
public:
    virtual ~IStm32RegisterBus() = default;

    /** Write @p len bytes starting at @p reg. */
    virtual bool writeReg(uint16_t reg, const uint8_t* data, size_t len) = 0;

    /** Read @p len bytes starting at @p reg. */
    virtual bool readReg(uint16_t reg, uint8_t* dst, size_t len) = 0;

    /** Convenience: single-byte write. */
    bool writeReg8(uint16_t reg, uint8_t value) {
        return writeReg(reg, &value, 1);
    }

    /** Convenience: single-byte read. */
    bool readReg8(uint16_t reg, uint8_t* out) {
        return readReg(reg, out, 1);
    }
};

} // namespace crosspad

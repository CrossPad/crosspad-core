// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include "crosspad/led/RgbColor.hpp"

namespace crosspad {

/**
 * @brief Abstract LED strip interface
 *
 * Platform implementations:
 *   - ESP-IDF: wraps led_strip RMT driver
 *   - Arduino: wraps Adafruit_NeoPixel or FastLED
 *   - Linux: stub / test double
 */
class ILedStrip {
public:
    virtual ~ILedStrip() = default;

    /// Initialize hardware
    virtual void begin() = 0;

    /// Set a single pixel color (buffered, call refresh() to push)
    virtual void setPixel(uint16_t idx, RgbColor color) = 0;

    /// Push buffered colors to the strip
    virtual void refresh() = 0;

    /// Number of LEDs on the strip
    virtual uint16_t getPixelCount() const = 0;

    /// Thread-safety: lock before multi-step LED operations.
    /// Default no-ops for single-threaded platforms.
    virtual void lock() {}
    virtual void unlock() {}
};

/// Platform-injected singleton (implemented by PlatformServices)
ILedStrip& getLedStrip();

} // namespace crosspad

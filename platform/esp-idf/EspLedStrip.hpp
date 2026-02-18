#pragma once

#include "crosspad/led/ILedStrip.hpp"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace crosspad {

/**
 * @brief ESP-IDF implementation of ILedStrip using RMT LED strip driver
 *
 * Supports optional pad-index → physical-LED-index remapping and
 * FreeRTOS mutex for thread-safe lock/unlock.
 */
class EspLedStrip : public ILedStrip {
public:
    struct Config {
        int gpioPin;
        uint16_t ledCount;
        uint32_t rmtResolutionHz = 10 * 1000 * 1000;
        bool useDma = true;
        const uint8_t* padToLedMap = nullptr;  // optional remap table
        uint8_t padCount = 0;                  // entries in remap table
    };

    explicit EspLedStrip(const Config& config);
    ~EspLedStrip() override;

    void begin() override;
    void setPixel(uint16_t idx, RgbColor color) override;
    void refresh() override;
    uint16_t getPixelCount() const override;
    void lock() override;
    void unlock() override;

    /// Direct access to underlying handle (escape hatch)
    led_strip_handle_t getHandle() const { return strip_; }

private:
    Config config_;
    led_strip_handle_t strip_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
    bool initialized_ = false;
};

} // namespace crosspad

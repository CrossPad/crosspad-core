#pragma once

#include "crosspad/led/ILedStrip.hpp"
#include "led_strip.h"

namespace crosspad {

/**
 * @brief ESP-IDF implementation of ILedStrip using RMT LED strip driver
 */
class EspLedStrip : public ILedStrip {
public:
    struct Config {
        int gpioPin;
        uint16_t ledCount;
        uint32_t rmtResolutionHz = 10 * 1000 * 1000;
        bool useDma = true;
    };

    explicit EspLedStrip(const Config& config);
    ~EspLedStrip() override;

    void begin() override;
    void setPixel(uint16_t idx, RgbColor color) override;
    void refresh() override;
    uint16_t getPixelCount() const override;

    /// Direct access to underlying handle (for ESP-IDF specific code)
    led_strip_handle_t getHandle() const { return strip_; }

private:
    Config config_;
    led_strip_handle_t strip_ = nullptr;
    bool initialized_ = false;
};

} // namespace crosspad

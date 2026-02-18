#include "EspLedStrip.hpp"
#include "esp_log.h"

static const char* TAG = "EspLedStrip";

namespace crosspad {

EspLedStrip::EspLedStrip(const Config& config) : config_(config) {}

EspLedStrip::~EspLedStrip() {
    if (strip_) {
        led_strip_del(strip_);
        strip_ = nullptr;
    }
}

void EspLedStrip::begin() {
    if (initialized_) return;

    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = config_.gpioPin;
    strip_config.max_leds = config_.ledCount;
    strip_config.led_model = LED_MODEL_WS2812;
    strip_config.flags.invert_out = false;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_config.resolution_hz = config_.rmtResolutionHz;
    rmt_config.mem_block_symbols = config_.useDma ? (config_.ledCount * 4) : 0;
    rmt_config.flags.with_dma = config_.useDma;

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &strip_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED strip: %s", esp_err_to_name(err));
        return;
    }

    led_strip_clear(strip_);
    initialized_ = true;
    ESP_LOGI(TAG, "LED strip initialized: %d LEDs on GPIO %d", config_.ledCount, config_.gpioPin);
}

void EspLedStrip::setPixel(uint16_t idx, RgbColor color) {
    if (!strip_ || idx >= config_.ledCount) return;
    led_strip_set_pixel(strip_, idx, color.R, color.G, color.B);
}

void EspLedStrip::refresh() {
    if (!strip_) return;
    led_strip_refresh(strip_);
}

uint16_t EspLedStrip::getPixelCount() const {
    return config_.ledCount;
}

} // namespace crosspad

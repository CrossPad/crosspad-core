#pragma once

#include <cstdint>
#include <cstdlib>

namespace crosspad {

struct EncoderAccelConfig {
    uint16_t fast_ms    = 50;   ///< Time window for fast acceleration (ms)
    uint16_t med_ms     = 90;   ///< Time window for medium acceleration (ms)
    uint8_t  fast_mult  = 8;    ///< Multiplier when fast threshold is reached
    uint8_t  med_mult   = 2;    ///< Multiplier when medium threshold is reached
    uint8_t  med_steps  = 6;    ///< Minimum accumulated steps for medium accel
    uint8_t  fast_steps = 10;   ///< Minimum accumulated steps for fast accel
};

/**
 * @brief Portable encoder acceleration algorithm.
 *
 * Tracks accumulated rotation steps within time windows and returns
 * a multiplier (1, med_mult, or fast_mult) based on rotation speed.
 *
 * Usage:
 *   EncoderAccelerator accel;
 *   int mult = accel.update(abs(diff), now_ms);
 *   int accelerated_diff = diff * mult;
 */
class EncoderAccelerator {
public:
    explicit EncoderAccelerator(EncoderAccelConfig config = {})
        : config_(config) {}

    /**
     * @brief Feed a new encoder movement and get the acceleration multiplier.
     *
     * @param abs_diff  Absolute encoder step count since last read (>0)
     * @param now_ms    Current timestamp in milliseconds
     * @return          Multiplier: 1 (normal), med_mult, or fast_mult
     */
    int update(int abs_diff, uint32_t now_ms) {
        uint32_t dt = (now_ms >= last_move_ms_) ? (now_ms - last_move_ms_) : 0;

        if (dt < config_.med_ms) {
            accel_count_ += abs_diff;
        } else {
            accel_count_ = abs_diff;
        }

        last_move_ms_ = now_ms;

        if (accel_count_ >= config_.fast_steps && dt < config_.fast_ms) {
            return config_.fast_mult;
        }
        if (accel_count_ >= config_.med_steps && dt < config_.med_ms) {
            return config_.med_mult;
        }
        return 1;
    }

private:
    EncoderAccelConfig config_;
    uint32_t last_move_ms_ = 0;
    int accel_count_ = 0;
};

} // namespace crosspad

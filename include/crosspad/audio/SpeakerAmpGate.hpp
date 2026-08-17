#pragma once

/**
 * @file SpeakerAmpGate.hpp
 * @brief Level-driven enable policy for the speaker amplifier.
 *
 * The class-D amp draws current whenever it is enabled, so it must stay off
 * while nothing is playing. Policy: enable as soon as the output bus carries
 * signal (the same peak the VU meter shows), keep it on while signal continues,
 * and drop it after a quiet hold window so short gaps between notes do not
 * click the amp on and off.
 *
 * Split across two contexts:
 *   - submitLevel() runs on the audio thread — lock-free, one atomic max.
 *   - poll() runs on a slow maintenance thread and decides the desired state;
 *     the caller performs the actual (blocking) hardware write.
 */

#include <atomic>
#include <cstdint>

namespace crosspad {

class SpeakerAmpGate {
public:
    struct Config {
        float    onThreshold  = 0.008f;   ///< peak (0..1) that switches the amp on
        float    offThreshold = 0.004f;   ///< peak below which the quiet timer runs
        uint32_t holdMs       = 4000;     ///< quiet time before the amp is dropped
    };

    /// Audio-thread side: report the loudest sample seen since the last poll.
    void submitLevel(float peak) {
        float prev = peakSincePoll_.load(std::memory_order_relaxed);
        while (peak > prev &&
               !peakSincePoll_.compare_exchange_weak(prev, peak,
                                                     std::memory_order_relaxed)) {
        }
    }

    /// User-facing master switch. False forces the amp off regardless of level.
    void setAllowed(bool allowed) { allowed_.store(allowed, std::memory_order_relaxed); }
    bool allowed() const { return allowed_.load(std::memory_order_relaxed); }

    void setConfig(const Config& cfg) { cfg_ = cfg; }

    /// Maintenance-thread side: returns the state the amp should be in now.
    bool poll(uint32_t nowMs) {
        const float peak = peakSincePoll_.exchange(0.0f, std::memory_order_relaxed);

        if (!allowed_.load(std::memory_order_relaxed)) {
            on_ = false;
            return false;
        }

        if (peak >= cfg_.onThreshold) {
            lastLoudMs_ = nowMs;
            on_ = true;
        } else if (on_ && peak < cfg_.offThreshold &&
                   (uint32_t)(nowMs - lastLoudMs_) >= cfg_.holdMs) {
            on_ = false;
        } else if (peak >= cfg_.offThreshold) {
            // Between the thresholds: treat as still playing, but do not turn on.
            lastLoudMs_ = nowMs;
        }
        return on_;
    }

    bool state() const { return on_; }

private:
    Config cfg_{};
    std::atomic<float> peakSincePoll_{0.0f};
    std::atomic<bool>  allowed_{true};
    bool     on_ = false;
    uint32_t lastLoudMs_ = 0;
};

} // namespace crosspad

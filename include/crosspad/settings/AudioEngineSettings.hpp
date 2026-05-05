// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace crosspad {

/**
 * @brief Audio engine config — applied at IAudioModule::setup().
 *
 * Restart-time only. Sample rate and frame count are not hot-swappable;
 * settings UI flags "reboot to apply" when these change.
 *
 * Allowed sample rates: 44100, 48000, 96000.
 * Frame count: power-of-two from 32 to 512 (latency vs CPU tradeoff).
 */
struct AudioEngineSettings {
    uint32_t sampleRate = 48000;
    uint32_t frameCount = 64;

    static constexpr uint32_t kSampleRates[]    = {44100, 48000, 96000};
    static constexpr uint8_t  kSampleRatesCount = 3;

    static constexpr uint32_t kFrameCounts[]    = {32, 48, 64, 128, 256, 512};
    static constexpr uint8_t  kFrameCountsCount = 6;

    static bool isValidSampleRate(uint32_t sr) {
        for (uint8_t i = 0; i < kSampleRatesCount; ++i) {
            if (kSampleRates[i] == sr) return true;
        }
        return false;
    }

    static bool isValidFrameCount(uint32_t fc) {
        for (uint8_t i = 0; i < kFrameCountsCount; ++i) {
            if (kFrameCounts[i] == fc) return true;
        }
        return false;
    }
};

} // namespace crosspad

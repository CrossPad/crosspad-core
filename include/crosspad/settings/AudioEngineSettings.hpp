// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace crosspad {

/**
 * @brief Audio engine config — applied at IAudioModule::setup().
 *
 * The sample rate is restart-time only. The frame count and the output queue
 * are applied live where the platform offers IAudioBuffer, at boot elsewhere.
 *
 * Allowed sample rates: 44100, 48000, 96000.
 * Frame count: 32 to 512 (latency vs CPU tradeoff).
 * Queue: frames between a render pass starting and its first frame reaching
 * the converter; 0 is the platform's own default.
 */
struct AudioEngineSettings {
    uint32_t sampleRate = 44100;
    uint32_t frameCount = 64;
    uint32_t queueFrames = 0;

    static constexpr uint32_t kSampleRates[]    = {44100, 48000, 96000};
    static constexpr uint8_t  kSampleRatesCount = 3;

    static constexpr uint32_t kFrameCounts[]    = {32, 48, 64, 128, 256, 512};
    static constexpr uint8_t  kFrameCountsCount = 6;

    /// Longest queue a settings file may ask for: 4096 frames, 93 ms at 44.1 kHz.
    static constexpr uint32_t kMaxQueueFrames = 4096;

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

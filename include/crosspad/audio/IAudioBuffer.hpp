// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace crosspad {

/// How far ahead of the converter the platform renders: the trade between a
/// pad's latency and the room a busy render pass has before the output runs
/// dry. One list of choices a user picks from, applied while playing, no
/// restart.
class IAudioBuffer {
public:
    struct Choice {
        /// Frames one render pass makes (AudioEngineSettings::frameCount).
        uint16_t blockFrames = 0;
        /// Frames between a pass starting and its first frame at the DAC
        /// (AudioEngineSettings::queueFrames).
        uint16_t queueFrames = 0;
    };

    virtual ~IAudioBuffer() = default;

    /// Choices, shortest wait first.
    virtual uint8_t  choices() const = 0;
    virtual Choice   choice(uint8_t i) const = 0;
    /// The choice running now.
    virtual uint8_t  current() const = 0;
    /// Switch to choice @p i while playing; kept across boots. False when the
    /// platform refused it and nothing changed.
    virtual bool     select(uint8_t i) = 0;
    /// The rate the frames above are at, to show them in milliseconds.
    virtual uint32_t sampleRate() const = 0;
};

} // namespace crosspad

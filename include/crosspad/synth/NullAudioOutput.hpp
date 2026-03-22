// SPDX-License-Identifier: MIT
#pragma once

#include "crosspad/synth/IAudioOutput.hpp"

namespace crosspad {

/**
 * @brief No-op IAudioOutput stub for platforms without audio hardware.
 *
 * Used as the default in PlatformServices until a real audio output
 * is registered via setAudioOutput().
 */
class NullAudioOutput : public IAudioOutput {
public:
    uint32_t write(const int16_t*, uint32_t frameCount) override { return frameCount; }
    uint32_t getSampleRate() const override { return 44100; }
    uint32_t getBufferSize() const override { return 256; }
};

} // namespace crosspad

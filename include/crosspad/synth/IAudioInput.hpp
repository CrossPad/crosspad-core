#pragma once
#include <cstdint>

namespace crosspad {

/**
 * @brief Abstract audio input interface.
 *
 * Platform implementations wrap the actual audio capture driver
 * (I2S, WASAPI, CoreAudio, ALSA, etc.).
 */
class IAudioInput {
public:
    virtual ~IAudioInput() = default;

    /// Read interleaved stereo samples (L,R,L,R,...). Returns frames actually read.
    virtual uint32_t read(int16_t* interleavedSamples, uint32_t frameCount) = 0;

    virtual uint32_t getSampleRate() const = 0;
    virtual uint32_t getBufferSize() const = 0;

    /// Get peak input levels for metering (resets after read).
    virtual void getInputLevel(int16_t& left, int16_t& right) const = 0;
};

} // namespace crosspad

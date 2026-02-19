#pragma once
#include <cstdint>

namespace crosspad {

/**
 * @brief Abstract audio output interface.
 *
 * Platform implementations wrap the actual audio driver
 * (I2S, WASAPI, CoreAudio, ALSA, WebAudio, etc.).
 */
class IAudioOutput {
public:
    virtual ~IAudioOutput() = default;

    /// Write interleaved stereo samples (L,R,L,R,...). Returns bytes written.
    virtual uint32_t write(const int16_t* interleavedSamples, uint32_t frameCount) = 0;

    virtual uint32_t getSampleRate() const = 0;
    virtual uint32_t getBufferSize() const = 0;
};

} // namespace crosspad

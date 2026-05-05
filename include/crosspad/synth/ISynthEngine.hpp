#pragma once
#include <cstdint>

namespace crosspad {

enum class SynthWaveform : uint8_t {
    Sine = 0,
    Saw,
    Square,
    Pulse,
    Triangle,
    Noise,
    Silence,
    Count
};

/**
 * @brief Abstract interface for a polyphonic synthesizer engine.
 *
 * Platform implementations provide the actual DSP, audio task,
 * and output device (I2S, WASAPI, CoreAudio, etc.).
 */
class ISynthEngine {
public:
    virtual ~ISynthEngine() = default;

    virtual void init() = 0;
    virtual void cleanup() = 0;

    // Note control
    virtual void noteOn(uint8_t note, uint8_t velocity) = 0;
    virtual void noteOff(uint8_t note) = 0;

    // Pitch bend (-8192 to +8191)
    virtual void setPitchBend(int16_t bend) = 0;

    // ADSR envelope
    virtual void setAttack(float value) = 0;   // 0.0-1.0
    virtual void setDecay(float value) = 0;     // 0.0-1.0
    virtual void setSustain(uint8_t value) = 0; // 0-127
    virtual uint8_t getSustain() = 0;
    virtual void setRelease(float value) = 0;   // 0.0-1.0

    // Filter
    virtual void setFilterCutoff(float value) = 0; // 0.0-1.0
    virtual void setFilterReso(float value) = 0;   // 0.0-1.0

    // Oscillator
    virtual void setWaveform(uint8_t osc, uint8_t waveformIdx) = 0;
    virtual void setOscVolume(uint8_t osc, float volume) = 0;
    virtual void setOscPitch(uint8_t osc, float pitch) = 0;

    // Delay
    virtual void setDelayEnabled(bool en) = 0;
    virtual void setDelayTime(float value) = 0;
    virtual void setDelayFeedback(float value) = 0;
    virtual void setDelayMix(float value) = 0;

    // Reverb
    virtual void setReverbEnabled(bool en) = 0;
    virtual void setReverbDecay(float value) = 0;
    virtual void setReverbMix(float value) = 0;

    // Level metering
    virtual void getLevel(int16_t& left, int16_t& right) = 0;

    // ── Float pipeline integration (preferred) ────────────────────
    // Render polyphonic synth output into an interleaved stereo float buffer
    // [-1.0, 1.0]. Default impl is a silence stub so older synth ports compile;
    // platform synth wrappers should override.
    virtual void process(float* stereoOut, uint32_t frames) {
        for (uint32_t i = 0; i < frames * 2; ++i) stereoOut[i] = 0.0f;
    }
};

} // namespace crosspad

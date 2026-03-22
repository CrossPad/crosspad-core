// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file IAudioModule.hpp
 * @brief Cross-platform audio processing pipeline interface.
 *
 * Models Marcel's audio_module pattern (P4): setup → process loop.
 * Each platform implements the hardware-specific parts (I2S, RtAudio, etc.)
 * while sharing the DSP pipeline structure.
 *
 * Typical usage:
 * @code
 *   // Platform creates and configures the module
 *   auto& mod = getAudioModule();
 *   mod.setup(config);
 *
 *   // Platform runs the process loop (FreeRTOS task, std::thread, etc.)
 *   while (running) {
 *       mod.process();   // pull inputs → DSP → push outputs
 *   }
 * @endcode
 *
 * Threading: The platform owns the audio thread. IAudioModule::process()
 * is called from that thread at the rate determined by the hardware/driver.
 */

#include <cstdint>
#include <cstddef>

namespace crosspad {

// ── Audio configuration ──────────────────────────────────────────────────

struct AudioModuleConfig {
    uint32_t sampleRate   = 48000;   ///< Sample rate in Hz
    uint32_t frameCount   = 48;      ///< Frames per process() call
    uint8_t  channelCount = 2;       ///< Channels per stream (typically 2 = stereo)
    uint8_t  streamCount  = 2;       ///< Number of independent output streams
};

// ── Audio stream interface ───────────────────────────────────────────────

/**
 * @brief A single audio output stream (e.g. one I2S interface or one RtAudio device).
 *
 * Platform implementations push interleaved samples to the hardware.
 * The IAudioModule calls pushSamples() once per process() cycle per stream.
 */
class IAudioStream {
public:
    virtual ~IAudioStream() = default;

    /// Push interleaved samples to this stream's output device.
    /// @param samples  Interleaved int32_t samples [frame][channel]
    /// @param frames   Number of frames to push
    /// @return frames actually written (may be less if non-blocking)
    virtual uint32_t pushSamples(const int32_t* samples, uint32_t frames) = 0;

    /// Pull interleaved samples from this stream's input device (optional).
    /// @param samples  Buffer to fill with interleaved int32_t samples
    /// @param frames   Number of frames to read
    /// @return frames actually read
    virtual uint32_t pullSamples(int32_t* samples, uint32_t frames) { (void)samples; (void)frames; return 0; }

    /// Set output volume (0-100). Implementation-dependent (codec register, software gain, etc.)
    virtual void setVolume(int volume) { (void)volume; }

    /// Get output volume (0-100).
    virtual int getVolume() const { return 100; }

    /// Is this stream open and functional?
    virtual bool isOpen() const = 0;
};

// ── Audio module interface ───────────────────────────────────────────────

/**
 * @brief Cross-platform audio processing pipeline.
 *
 * The module owns the DSP pipeline and delegates I/O to IAudioStream instances.
 * Platforms implement this to provide their specific DSP (synth, sampler, effects)
 * and wire up the streams (I2S, RtAudio, etc.).
 *
 * Process cycle (mirrors Marcel's audio_module.cpp):
 *   1. pullInputs()    — read from input streams into DSP buffers
 *   2. generate()      — run DSP (synthesis, sample playback, effects)
 *   3. control()       — runtime control logic (volume ramps, mute after timeout)
 *   4. pushOutputs()   — interleave DSP buffers and write to output streams
 */
class IAudioModule {
public:
    virtual ~IAudioModule() = default;

    /// One-time setup. Returns false on failure.
    virtual bool setup(const AudioModuleConfig& config) = 0;

    /// Single DSP cycle: pull → generate → control → push.
    /// Called from the audio thread in a tight loop.
    virtual void process() = 0;

    /// Tear down and release resources.
    virtual void teardown() {}

    // ── Configuration queries ────────────────────────────────────

    virtual const AudioModuleConfig& getConfig() const = 0;

    // ── Stream management ────────────────────────────────────────

    /// Get an output stream by index (0..streamCount-1).
    virtual IAudioStream* getOutputStream(uint8_t index) = 0;

    /// Get an input stream by index (optional, may return nullptr).
    virtual IAudioStream* getInputStream(uint8_t index) { (void)index; return nullptr; }

    // ── Level metering ───────────────────────────────────────────

    /// Get peak output level for a stream (0..streamCount-1).
    /// Values are abs(sample) peaks since last call, suitable for VU meters.
    virtual void getOutputLevel(uint8_t stream, int16_t& left, int16_t& right) const {
        (void)stream; left = 0; right = 0;
    }

    /// Get peak input level for a stream.
    virtual void getInputLevel(uint8_t stream, int16_t& left, int16_t& right) const {
        (void)stream; left = 0; right = 0;
    }
};

} // namespace crosspad

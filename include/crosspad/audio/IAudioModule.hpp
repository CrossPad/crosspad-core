// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file IAudioModule.hpp
 * @brief Cross-platform audio processing pipeline.
 *
 * Internal pipeline format is float32 in [-1.0, 1.0]. Streams declare which
 * formats they natively support (Float32 / Int32 / Int16) — the module picks
 * the highest-quality match and converts only at the I/O boundary.
 *
 * Sample rate is configured at setup() time from CrosspadSettings.audioEngine
 * and is not hot-swappable; restart required to change. Nodes that depend on
 * SR rebuild their state in onSampleRateChange().
 *
 * Pipeline cycle (one process() call):
 *   1. pull inputs into float bus buffer (if any)
 *   2. for each generator node: node.mix(bus, frames)
 *   3. for each effect node:    node.process(bus, bus, frames)
 *   4. push bus to each output stream (with format conversion if needed)
 */

#include "crosspad/audio/AudioFormat.hpp"

#include <cstdint>
#include <cstddef>

namespace crosspad {

class IAudioNode;

// ── Audio configuration ──────────────────────────────────────────────────

struct AudioModuleConfig {
    uint32_t sampleRate   = 48000;
    uint32_t frameCount   = 64;
    uint8_t  channelCount = 2;
    uint8_t  streamCount  = 2;
};

// ── Audio stream interface ───────────────────────────────────────────────

/**
 * @brief A single audio I/O endpoint (one I2S interface, one RtAudio device).
 *
 * Streams declare format support via supportedFormats() bitmask; the module
 * picks the highest-quality match (Float32 > Int32 > Int16). Push/pull take
 * a runtime AudioFormat tag so the same call signature works for any format.
 */
class IAudioStream {
public:
    virtual ~IAudioStream() = default;

    /// Bitmask of supported AudioFormat values. Must include at least one.
    virtual uint32_t supportedFormats() const = 0;

    /// Push interleaved samples in the given format.
    /// @param samples  Buffer holding `frames * channelCount` values of the
    ///                 element size implied by `fmt` (e.g. for stereo Int16,
    ///                 `frames * 2 * sizeof(int16_t)` bytes).
    /// @param fmt      Element format of `samples`.
    /// @param frames   Number of multi-channel frames (one frame = one
    ///                 sample per channel; "stereo frames" for 2-ch streams).
    /// @return         Frames actually accepted; may be < frames on backpressure.
    virtual uint32_t pushSamples(const void* samples, AudioFormat fmt, uint32_t frames) = 0;

    /// Pull interleaved samples in the given format. See pushSamples for
    /// `frames` semantics. Returns frames actually delivered.
    virtual uint32_t pullSamples(void* samples, AudioFormat fmt, uint32_t frames) {
        (void)samples; (void)fmt; (void)frames; return 0;
    }

    virtual void setVolume(int volume) { (void)volume; }
    virtual int getVolume() const { return 100; }

    virtual bool isOpen() const = 0;
    virtual uint32_t getSampleRate() const = 0;
};

// ── Audio module interface ───────────────────────────────────────────────

class IAudioModule {
public:
    virtual ~IAudioModule() = default;

    virtual bool setup(const AudioModuleConfig& config) = 0;
    virtual void teardown() {}

    /// One DSP cycle: pull inputs → mix generators → process effects → push outputs.
    /// Called from the audio thread (FreeRTOS task on embedded, std::thread on PC).
    virtual void process() = 0;

    virtual const AudioModuleConfig& getConfig() const = 0;

    // ── Node chain ───────────────────────────────────────────────

    /// Append a node to the chain. Node ownership stays with caller.
    virtual void addNode(IAudioNode* node) = 0;
    virtual void removeNode(IAudioNode* node) = 0;
    virtual size_t nodeCount() const = 0;
    virtual IAudioNode* getNode(size_t index) const = 0;

    // ── Streams ──────────────────────────────────────────────────

    virtual IAudioStream* getOutputStream(uint8_t index) = 0;
    virtual IAudioStream* getInputStream(uint8_t /*index*/) { return nullptr; }

    // ── Level metering (float [0.0, 1.0]) ────────────────────────

    virtual void getOutputLevel(uint8_t /*stream*/, float& left, float& right) const {
        left = 0.0f; right = 0.0f;
    }

    virtual void getInputLevel(uint8_t /*stream*/, float& left, float& right) const {
        left = 0.0f; right = 0.0f;
    }
};

/// Platform-injected singleton (implemented by PlatformServices)
IAudioModule& getAudioModule();

} // namespace crosspad

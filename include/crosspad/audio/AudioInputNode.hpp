// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file AudioInputNode.hpp
 * @brief IAudioNode generator that pulls from an IAudioInput.
 *
 * Reads interleaved int16 stereo from the input device, scales to float
 * [-1.0, 1.0] and adds (additive mix) into the bus. Used to plug PC virtual
 * inputs (Pulse capture, RtAudio device) or future embedded ADC streams into
 * the AudioMixerEngine via the unified IAudioNode contract.
 */

#include "crosspad/audio/IAudioNode.hpp"
#include "crosspad/audio/AudioFormatConvert.hpp"
#include "crosspad/synth/IAudioInput.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

namespace crosspad {

class AudioInputNode : public IAudioNode {
public:
    /// Indexed getter form — resolves the IAudioInput at every render.
    /// Use when the platform hot-swaps input devices (e.g. virtual sinks).
    using IndexedGetter = IAudioInput*(*)(int);

    AudioInputNode(IAudioInput* input, const char* nodeName)
        : input_(input), name_(nodeName) {}

    AudioInputNode(IndexedGetter getter, int index, const char* nodeName)
        : getter_(getter), getterIndex_(index), name_(nodeName) {}

    AudioInputNode() : AudioInputNode(static_cast<IAudioInput*>(nullptr), "AudioInput") {}

    void setInput(IAudioInput* input) { input_ = input; }
    IAudioInput* getInput() const {
        return getter_ ? getter_(getterIndex_) : input_;
    }

    const char* name() const override { return name_; }
    bool isGenerator() const override { return true; }

    void onPrepare(uint32_t maxFrames, uint32_t /*sampleRate*/) override {
        // Input device owns its own SR; pre-size the int16 scratch so mix()
        // never allocates on the audio thread.
        rawBuf_.assign(static_cast<size_t>(maxFrames) * 2, 0);
    }

    void mix(float* out, uint32_t frames) override {
        IAudioInput* in = getter_ ? getter_(getterIndex_) : input_;
        if (!in) return;
        const uint32_t stereo = frames * 2;

        uint32_t got = in->read(rawBuf_.data(), frames);
        if (got == 0) return;
        if (got < frames) {
            std::memset(rawBuf_.data() + got * 2, 0, (frames - got) * 2 * sizeof(int16_t));
        }

        for (uint32_t i = 0; i < stereo; ++i) {
            out[i] += rawBuf_[i] * kInt16ScaleToFloat;
        }
    }

private:
    IAudioInput*   input_       = nullptr;
    IndexedGetter  getter_      = nullptr;
    int            getterIndex_ = 0;
    const char*    name_        = "AudioInput";
    std::vector<int16_t> rawBuf_;
};

} // namespace crosspad

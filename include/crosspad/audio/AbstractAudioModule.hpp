// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file AbstractAudioModule.hpp
 * @brief Default IAudioModule implementation with float bus + node chain.
 *
 * Concrete platform modules (IdfAudioModule, ArduinoAudioModule, PcAudioModule)
 * inherit from this and provide:
 *   - getOutputStream() / getInputStream() (hardware streams)
 *   - thread management (FreeRTOS task or std::thread that calls process())
 *
 * The default process() handles:
 *   - clearing the float bus buffer
 *   - calling generator nodes' mix()
 *   - calling effect nodes' process() in chain order
 *   - pushing the bus to all output streams with format conversion
 *   - peak metering (float [0.0, 1.0])
 */

#include "crosspad/audio/IAudioModule.hpp"
#include "crosspad/audio/IAudioNode.hpp"
#include "crosspad/audio/AudioFormatConvert.hpp"

#include <vector>
#include <algorithm>
#include <cstring>

namespace crosspad {

class AbstractAudioModule : public IAudioModule {
public:
    bool setup(const AudioModuleConfig& config) override {
        config_ = config;
        const size_t samples = static_cast<size_t>(config.frameCount) * config.channelCount;
        bus_.assign(samples, 0.0f);
        scratchInt16_.assign(samples, 0);
        scratchInt32_.assign(samples, 0);
        for (auto* n : nodes_) if (n) n->onSampleRateChange(config.sampleRate);
        return true;
    }

    void teardown() override {
        bus_.clear();
        scratchInt16_.clear();
        scratchInt32_.clear();
    }

    void process() override {
        const uint32_t frames  = config_.frameCount;
        const uint32_t samples = frames * config_.channelCount;

        // 1. clear bus
        std::memset(bus_.data(), 0, samples * sizeof(float));

        // 2. generator nodes mix into bus, 3. effect nodes process bus in-place
        for (auto* node : nodes_) {
            if (!node || !node->enabled()) continue;
            if (node->isGenerator()) {
                node->mix(bus_.data(), frames);
            } else {
                node->process(bus_.data(), bus_.data(), frames);
            }
        }

        // 4. peak metering on bus
        updateLevels(bus_.data(), frames);

        // 5. push to all output streams
        for (uint8_t s = 0; s < config_.streamCount; ++s) {
            IAudioStream* stream = getOutputStream(s);
            if (!stream || !stream->isOpen()) continue;
            pushBusToStream(stream, frames);
        }
    }

    const AudioModuleConfig& getConfig() const override { return config_; }

    void addNode(IAudioNode* node) override {
        if (!node) return;
        nodes_.push_back(node);
        if (config_.sampleRate > 0) node->onSampleRateChange(config_.sampleRate);
    }

    void removeNode(IAudioNode* node) override {
        nodes_.erase(std::remove(nodes_.begin(), nodes_.end(), node), nodes_.end());
    }

    size_t nodeCount() const override { return nodes_.size(); }

    IAudioNode* getNode(size_t index) const override {
        return index < nodes_.size() ? nodes_[index] : nullptr;
    }

    void getOutputLevel(uint8_t /*stream*/, float& left, float& right) const override {
        left  = peakL_;
        right = peakR_;
    }

protected:
    AudioModuleConfig config_{};
    std::vector<IAudioNode*> nodes_;
    std::vector<float>   bus_;
    std::vector<int16_t> scratchInt16_;
    std::vector<int32_t> scratchInt32_;

    float peakL_ = 0.0f;
    float peakR_ = 0.0f;

private:
    void pushBusToStream(IAudioStream* stream, uint32_t frames) {
        const uint32_t samples = frames * config_.channelCount;
        const AudioFormat fmt = audioFormatPreferred(stream->supportedFormats());

        switch (fmt) {
            case AudioFormat::Float32:
                stream->pushSamples(bus_.data(), AudioFormat::Float32, frames);
                break;
            case AudioFormat::Int32:
                floatToInt32(bus_.data(), scratchInt32_.data(), samples);
                stream->pushSamples(scratchInt32_.data(), AudioFormat::Int32, frames);
                break;
            case AudioFormat::Int16:
                floatToInt16(bus_.data(), scratchInt16_.data(), samples);
                stream->pushSamples(scratchInt16_.data(), AudioFormat::Int16, frames);
                break;
        }
    }

    void updateLevels(const float* buf, uint32_t frames) {
        constexpr float kDecay = 0.93f;
        peakL_ *= kDecay;
        peakR_ *= kDecay;
        for (uint32_t i = 0; i < frames; ++i) {
            float l = buf[i * 2 + 0];
            float r = buf[i * 2 + 1];
            if (l < 0) l = -l;
            if (r < 0) r = -r;
            if (l > peakL_) peakL_ = l;
            if (r > peakR_) peakR_ = r;
        }
    }
};

} // namespace crosspad

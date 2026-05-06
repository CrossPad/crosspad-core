// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file SynthEngineNode.hpp
 * @brief IAudioNode generator that defers to an ISynthEngine.
 *
 * Header-only adapter: wraps any ISynthEngine into the audio module's node
 * chain. Calls ISynthEngine::process(float*, frames) into a local scratch
 * buffer, then mixes (additive) into the bus. Use this when the platform
 * synth implements ISynthEngine — no need to write a per-platform IAudioNode.
 */

#include "crosspad/audio/IAudioNode.hpp"
#include "crosspad/synth/ISynthEngine.hpp"

#include <cstdint>
#include <vector>

namespace crosspad {

class SynthEngineNode : public IAudioNode {
public:
    explicit SynthEngineNode(ISynthEngine* engine = nullptr) : engine_(engine) {}

    void setEngine(ISynthEngine* engine) { engine_ = engine; }
    ISynthEngine* getEngine() const { return engine_; }

    const char* name() const override { return "SynthEngine"; }
    bool isGenerator() const override { return true; }

    void onPrepare(uint32_t maxFrames, uint32_t /*sampleRate*/) override {
        scratch_.assign(static_cast<size_t>(maxFrames) * 2, 0.0f);
    }

    void mix(float* out, uint32_t frames) override {
        if (!engine_) return;
        const uint32_t samples = frames * 2;
        // ISynthEngine::process is a write (=) contract; scratch overwritten fully.
        // Buffer pre-sized in onPrepare(); never grow on the audio thread.
        engine_->process(scratch_.data(), frames);
        for (uint32_t i = 0; i < samples; ++i) out[i] += scratch_[i];
    }

private:
    ISynthEngine* engine_;
    std::vector<float> scratch_;
};

} // namespace crosspad

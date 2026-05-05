// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace crosspad {

/**
 * @brief A DSP unit in the audio module's node chain.
 *
 * Nodes operate on interleaved stereo float32 buffers in [-1.0, 1.0].
 * Generators (synths, samplers) override mix() to add into the bus buffer.
 * Effects override process() and may be in-place (in == out is allowed).
 *
 * Sample rate is owned by the IAudioModule. Nodes that depend on SR
 * (filter coefficients, delay buffers) must implement onSampleRateChange()
 * which the module calls during setup() and on any restart-time SR change.
 */
class IAudioNode {
public:
    virtual ~IAudioNode() = default;

    /// Effect-style processing. Interleaved stereo [L,R,L,R,...] in [-1.0, 1.0].
    /// Default impl is pass-through; override for actual DSP.
    virtual void process(const float* in, float* out, uint32_t frames) {
        if (in != out) {
            for (uint32_t i = 0; i < frames * 2; ++i) out[i] = in[i];
        }
    }

    /// Generator-style mixing. Adds into existing bus buffer; default no-op.
    virtual void mix(float* /*out*/, uint32_t /*frames*/) {}

    /// Marks this node as a generator (mix() called) vs effect (process() called).
    virtual bool isGenerator() const { return false; }

    virtual const char* name() const = 0;
    virtual bool enabled() const { return true; }
    virtual void setEnabled(bool /*on*/) {}

    /// Called by IAudioModule when sample rate is established or changed.
    /// Nodes rebuild SR-dependent state here.
    virtual void onSampleRateChange(uint32_t /*newSr*/) {}
};

} // namespace crosspad

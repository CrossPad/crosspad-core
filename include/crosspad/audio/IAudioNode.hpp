// SPDX-License-Identifier: GPL-3.0-or-later
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
 * ── Lifecycle ─────────────────────────────────────────────────────────────
 * 1. Constructed on any thread.
 * 2. Registered into a module/mixer (addNode / addChannel).
 * 3. Module/mixer calls onPrepare(maxFrames, sampleRate) once during setup
 *    and again on any restart-time change. This is where SR-dependent state
 *    (filter coefficients, delay lines, scratch buffers sized to maxFrames)
 *    must be allocated.
 * 4. Audio thread calls mix() / process() repeatedly, never beyond maxFrames.
 *
 * ── RT-safety contract for mix() / process() ─────────────────────────────
 * These run on the audio thread and MUST be real-time safe:
 *   - No heap allocation (no new, no vector::resize, no string formatting).
 *   - No blocking primitives (no mutex, no condvar, no ring-buffer wait).
 *   - No syscalls / logging / file I/O.
 *   - No printf-family or any unbounded code path.
 * Communicate with non-audio threads via std::atomic on POD scalars or
 * lock-free SPSC queues. Any setup-time work belongs in onPrepare().
 *
 * setEnabled() / enabled() and any setter exposed to UI code may be called
 * from any thread; back them with std::atomic.
 */
class IAudioNode {
public:
    virtual ~IAudioNode() = default;

    /// Effect-style processing. Interleaved stereo [L,R,L,R,...] in [-1.0, 1.0].
    /// Default impl is pass-through; override for actual DSP. RT-safe contract
    /// applies — see class doc.
    virtual void process(const float* in, float* out, uint32_t frames) {
        if (in != out) {
            for (uint32_t i = 0; i < frames * 2; ++i) out[i] = in[i];
        }
    }

    /// Generator-style mixing. Adds into existing bus buffer; default no-op.
    /// RT-safe contract applies — see class doc.
    virtual void mix(float* /*out*/, uint32_t /*frames*/) {}

    /// Marks this node as a generator (mix() called) vs effect (process() called).
    virtual bool isGenerator() const { return false; }

    virtual const char* name() const = 0;
    virtual bool enabled() const { return true; }
    virtual void setEnabled(bool /*on*/) {}

    /// Called by the host (IAudioModule or AudioMixerEngine) once both the
    /// audio buffer size and sample rate are known, and again on any
    /// restart-time change. Allocate scratch buffers sized to maxFrames * 2
    /// (stereo) here so subsequent mix()/process() calls stay alloc-free.
    /// Default impl forwards to legacy onSampleRateChange() for nodes that
    /// only care about sample rate.
    virtual void onPrepare(uint32_t /*maxFrames*/, uint32_t sampleRate) {
        onSampleRateChange(sampleRate);
    }

    /// Legacy SR-only notification. Prefer overriding onPrepare() for new
    /// nodes that need the buffer size for pre-allocation.
    virtual void onSampleRateChange(uint32_t /*newSr*/) {}
};

} // namespace crosspad

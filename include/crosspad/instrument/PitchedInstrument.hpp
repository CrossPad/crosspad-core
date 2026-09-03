// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file PitchedInstrument.hpp
 * @brief Varispeed sample engine: every pad plays one recorded zone transposed.
 *
 * A generator IAudioNode. Voices read their zone with a 32.32 fixed-point
 * phase and 4-point cubic Hermite interpolation, so the per-sample cost is
 * O(1) and 16+ voices fit the 64-frame block the RT task allows. Duration
 * scales with pitch, as on any classic sampler.
 *
 * Threading: three tasks touch this object. The audio RT task calls mix(); the
 * event-bus task calls noteOn()/noteOff(); the kit-load worker swaps the bank.
 * The worker brackets its work in beginMutation()/endMutation(), which shuts
 * the note path off, and holds the RT task out with the render gate for the
 * same window. installBank() is that whole sequence, and is what a platform
 * should call; the pieces are public because a port may need to drive them
 * one at a time (an unload, say).
 *
 * mix() allocates nothing and takes no lock.
 *
 * Every block also lands in scopeTap() as a mono sum, for a waveform view.
 */

#include "crosspad/audio/IAudioNode.hpp"
#include "crosspad/audio/ScopeTap.hpp"
#include "crosspad/instrument/SampleBank.hpp"
#include "crosspad/kit/KitInfo.hpp"

#include <atomic>
#include <cstdint>
#include <vector>

#if defined(CROSSPAD_USE_ESP_DSP) && __has_include(<esp_dsp.h>)
  #include <esp_dsp.h>
  #ifndef CROSSPAD_HAS_ESP_DSP
    #define CROSSPAD_HAS_ESP_DSP 1
  #endif
#else
  #ifndef CROSSPAD_HAS_ESP_DSP
    #define CROSSPAD_HAS_ESP_DSP 0
  #endif
#endif

namespace crosspad {

struct InstrumentParams {
    uint8_t maxVoices = 24;
    float attackMs = 2.0f;
    float releaseMs = 150.0f;
};

namespace detail {

/**
 * @brief out[L,R] += mono * (gainL, gainR), interleaved stereo.
 *
 * @param tmp scratch of at least @p frames floats (the ESP-DSP path needs a
 *            unit-stride intermediate; the scalar path ignores it).
 */
inline void accumulatePanned(const float* mono, float* out, uint32_t frames,
                             float gainL, float gainR, float* tmp) {
#if CROSSPAD_HAS_ESP_DSP
    const int n = static_cast<int>(frames);
    dsps_mulc_f32(mono, tmp, n, gainL, 1, 1);
    dsps_add_f32(out, tmp, out, n, 2, 1, 2);
    dsps_mulc_f32(mono, tmp, n, gainR, 1, 1);
    dsps_add_f32(out + 1, tmp, out + 1, n, 2, 1, 2);
#else
    (void)tmp;
    for (uint32_t i = 0; i < frames; ++i) {
        out[2 * i]     += mono[i] * gainL;
        out[2 * i + 1] += mono[i] * gainR;
    }
#endif
}

} // namespace detail

class PitchedInstrument : public IAudioNode {
public:
    static constexpr uint8_t kMaxVoices = 32;
    static constexpr uint8_t kPads = 16;

    PitchedInstrument();

    /// Publish a bank to the audio thread. nullptr silences the engine.
    void setBank(SampleBank* bank);
    SampleBank* bank() const { return bank_.load(std::memory_order_acquire); }

    void setParams(const InstrumentParams& p);
    void setPadMix(uint8_t pad, int volume, int pan);

    /**
     * @brief Close the note path so the loader can retire the current bank.
     *
     * Between these two calls noteOn() and noteOff() do nothing, so no voice
     * can take a fresh reference into the bank that is about to be freed.
     * Re-entrancy is not supported: one mutator at a time.
     */
    void beginMutation();
    void endMutation();

    /**
     * @brief Hold the RT task out of the engine until gateExit().
     *
     * A flag, not a lock: mix() never blocks, it simply contributes nothing
     * while the gate is held. That is what makes freeing the previous bank on
     * the loader thread safe.
     *
     * **True only when the RT task was observed outside mix()** — that
     * observation is the whole safety argument for freeing what the gate
     * protects. On false the gate is still held (gateExit() is still owed),
     * but the renderer never answered, so nothing it could still be reading
     * may be released.
     *
     * @param maxSpins how many times to look before giving up.
     * @param yield    called between looks; null busy-spins. On FreeRTOS pass
     *                 a one-tick delay — one 64-frame block is 1.45 ms at
     *                 44.1 kHz, so ten ticks is two orders of magnitude of
     *                 slack, and past that the RT task is not running at all.
     * Never call from the audio thread.
     */
    bool gateEnter(uint32_t maxSpins, void (*yield)());
    void gateExit();

    /**
     * @brief Swap in a new bank: gate, mutate, publish, let the renderer back.
     *
     * gate -> beginMutation -> allNotesOff -> setBank -> setParams ->
     * setPadMix -> endMutation -> gateExit.
     *
     * @param padMixFrom kit whose per-pad volume/pan to apply; null keeps the
     *        current mix.
     * @param safeToFree receives what gateEnter() answered. **The returned
     *        bank may only be freed when it is true**; on false the renderer
     *        never acknowledged the gate and may still hold the old PCM, so a
     *        leak is the correct trade against a use-after-free under a live
     *        render. May be null if the caller does not intend to free.
     * @return the bank that was installed before, or nullptr if there was none.
     */
    SampleBank* installBank(SampleBank* bank, const InstrumentParams& p,
                            const KitInfo* padMixFrom, uint32_t maxSpins,
                            void (*yield)(), bool* safeToFree);

    /// @param pad grid position the note came from, for its volume/pan. Any
    ///        value >= kPads means "not from the grid" (MIDI in) and takes the
    ///        unattenuated, centred mix.
    void noteOn(uint8_t midiNote, uint8_t velocity, uint8_t pad);
    void noteOff(uint8_t midiNote);
    void allNotesOff();

    // ── IAudioNode ──
    const char* name() const override { return "Pitched"; }
    bool isGenerator() const override { return true; }
    void onPrepare(uint32_t maxFrames, uint32_t sampleRate) override;
    void mix(float* out, uint32_t frames) override;

    /**
     * @brief The engine's own output, mono, as a UI thread can read it.
     *
     * Written once per mix() — including the all-zero blocks between notes, so
     * a view decays to a flat line instead of freezing on the last waveform
     * that happened to be loud. Pre-pan and pre-master: this is what the
     * instrument produced, not what came out of the mixer.
     */
    ScopeTap& scopeTap() { return scopeTap_; }
    const ScopeTap& scopeTap() const { return scopeTap_; }

    /**
     * @brief Fundamental of the most recent note that actually started a voice.
     *
     * 440 * 2^((note - 69) / 12); 0 before any note. A view sizes its window
     * from this, so a note that was dropped (no bank, mutation in flight) must
     * not move it — the window would then describe a wave that is not there.
     */
    float lastNoteHz() const { return lastNoteHz_.load(std::memory_order_relaxed); }

    // ── telemetry ──
    uint8_t activeVoices() const;
    uint32_t stealCount() const { return steals_.load(std::memory_order_relaxed); }
    void resetStats() { steals_.store(0, std::memory_order_relaxed); }

private:
    struct Voice {
        const Zone* zone = nullptr;
        uint64_t phase = 0;        ///< 32.32 position inside the zone
        uint64_t inc = 0;          ///< 32.32 samples per output frame
        uint64_t order = 0;        ///< allocation sequence, for oldest-first stealing
        float env = 0.0f;
        float attackStep = 0.0f;   ///< 0 means "no attack ramp"
        float releaseStep = 0.0f;
        float gain = 0.0f;         ///< velocity * pad volume, before the envelope
        float gainL = 0.0f;
        float gainR = 0.0f;
        uint8_t note = 0;
        /// Publication flag: everything above is valid once this reads true.
        std::atomic<bool> active{false};
        bool releasing = false;

        /**
         * @brief Silence the voice without dropping its zone.
         *
         * `zone` is deliberately left alone. The RT task can be inside
         * renderVoice() on this slot right now, holding the pointer it read
         * when it saw `active`; nulling the field under it would fault. A
         * cleared voice is simply never rendered again, and the next noteOn()
         * overwrites `zone` before it republishes `active`.
         */
        void reset() {
            active.store(false, std::memory_order_relaxed);
            releasing = false;
            env = 0.0f;
        }
    };

    int findVoiceSlot();
    void renderVoice(Voice& v, uint32_t frames);
    /// mix() minus the render gate — everything the audio thread actually does.
    void renderBlock(float* out, uint32_t frames);
    /// Push the block's mono sum to the tap. @p silent zeroes it first, for
    /// the paths that produced no audio at all.
    void publishScope(uint32_t frames, bool silent);

    std::atomic<SampleBank*> bank_{nullptr};
    std::atomic<bool> mutating_{false};
    std::atomic<bool> gated_{false};
    std::atomic<bool> rendering_{false};
    std::atomic<uint8_t> maxVoices_{24};
    std::atomic<float> attackMs_{2.0f};
    std::atomic<float> releaseMs_{150.0f};
    std::atomic<int16_t> padVolume_[kPads];
    std::atomic<int16_t> padPan_[kPads];
    std::atomic<uint32_t> steals_{0};
    std::atomic<float> lastNoteHz_{0.0f};

    Voice voices_[kMaxVoices];
    uint64_t nextOrder_ = 1;

    uint32_t sampleRate_ = 44100;
    uint32_t maxFrames_ = 0;
    std::vector<float> scratch_;   ///< per-voice mono render
    std::vector<float> panTmp_;    ///< unit-stride intermediate for ESP-DSP
    std::vector<float> monoSum_;   ///< block sum of every voice, for the tap
    ScopeTap scopeTap_;
};

} // namespace crosspad

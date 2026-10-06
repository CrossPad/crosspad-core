// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file SamplerEngine.hpp
 * @brief Sixteen pads, streamed from the card, pitched in real time.
 *
 * A generator `IAudioNode`: `mix()` adds into the interleaved stereo float
 * block the mixer has just cleared. It allocates nothing, takes no lock,
 * opens no file and writes no log -- the budget is 1.45 ms for 64 frames at
 * 44.1 kHz, and `MetronomeNode` counts the sequencer's beat in frames
 * *rendered*, so a render that overruns does not thin the sound, it takes the
 * pulse with it.
 *
 * ## Three threads, and a voice state each one owns
 *
 * The engine this replaces had no thread safety at all, and every bit of
 * arbitration had to be invented around it -- a render gate, a file mutex,
 * three per-task file slots, a parking array for handles the render thread was
 * not allowed to close. Here the ownership is in the states themselves, and
 * a voice moves through them in one direction only:
 *
 * ```
 *   Free  --(note thread: claim, fill, publish)-->  Playing
 *   Playing --(render thread: it ended, or it was stolen and faded)--> Finished
 *   Finished --(streaming thread: give the blocks back)--> Free
 * ```
 *
 * Each arrow has exactly one writer, so the only atomic operation needed is
 * the claim, where two note sources could race for the same slot. In
 * particular **the render thread never gives a block back and never closes
 * anything** -- it marks the voice Finished and moves on, which is the same
 * rule the file port had to discover the hard way about `fclose()`.
 *
 * ## Stealing
 *
 * Above the voice cap the oldest voice is asked to fade, over
 * `kStealFadeMs`, in the slot it already occupies -- its phase and envelope
 * are in there. So the note that stole it needs a different slot, and
 * `Params::maxVoices` is held `kFadeSlots` below `kMaxVoices` to guarantee
 * one. A hard cut at an arbitrary phase is a click at full scale, the one
 * artefact a sampler cannot hide.
 *
 * ## What a note-on costs
 *
 * Nothing on the card. The sample's head is resident (`SampleStore`), so
 * starting a voice is filling a struct and a store-release. What streams is
 * only what the voice will reach later, and `StreamWindow` is opened in the
 * same call -- off the render thread, out of a pool, with a refusal (rather
 * than a wait) when the pool is empty.
 */

#include "crosspad/audio/IAudioNode.hpp"
#include "crosspad/sampler/BlockPool.hpp"
#include "crosspad/sampler/SampleCursor.hpp"
#include "crosspad/sampler/SampleStore.hpp"
#include "crosspad/sampler/SamplerKit.hpp"
#include "crosspad/sampler/StreamWindow.hpp"

#include <atomic>
#include <cstdint>

namespace crosspad {

class SamplerEngine : public IAudioNode {
public:
    static constexpr uint8_t kMaxVoices = 32;
    static constexpr uint8_t kFadeSlots = 4;
    static constexpr uint8_t kMaxSounding = kMaxVoices - kFadeSlots;
    static constexpr uint8_t kPads = SamplerKit::kPads;
    static constexpr float   kStealFadeMs = 2.0f;

    struct Params {
        /// Voices that may sound at once. Sixteen is what the engine this
        /// replaces was capped at, and lowering that cap made a player's
        /// timing worse, not better: the cost is in the steal, not the voice.
        uint8_t maxVoices = 16;
        /// A rise this short is inaudible and removes the click of starting
        /// mid-waveform on a sample whose first frame is not zero.
        float   attackMs  = 0.5f;
        /// What a release in Hold mode takes.
        float   releaseMs = 8.0f;
        /**
         * @brief Stop a one-shot once it has been inaudible this long.
         *
         * A drum hit's file is mostly tail, and the tail is mostly silence: a
         * 2.5 s sample of a 0.5 s hit spends two seconds streaming nothing
         * from the card. The engine this replaces never had the problem
         * because its per-voice buffer simply ended at 0.56 s and the rest of
         * the file was never played at all -- a truncation, not a decision.
         *
         * Here the whole file plays, and a voice that has gone quiet is
         * stopped instead. Measured on this author's CLASSIC kit: thirteen
         * voices alive at twenty hits a second, where the audible part of a
         * hit is over in half a second.
         *
         * Only one-shots. A loop is meant to carry on, and a held note is the
         * player's to release. 0 disables it.
         */
        float   silenceMs = 200.0f;
        /// What counts as inaudible, as a fraction of full scale. -80 dBFS.
        float   silenceLevel = 1.0e-4f;
    };

    struct Counters {
        uint32_t notesStarted  = 0;
    uint32_t silenced      = 0;   ///< one-shots stopped because they had gone quiet
        uint32_t notesDropped  = 0;   ///< no voice free, not even to steal
        uint32_t steals        = 0;
        uint32_t streamRefused = 0;   ///< the pool had no blocks: the voice plays its head only
        uint32_t underruns     = 0;   ///< a voice reached frames the streamer had not delivered
        uint32_t voicesPeak    = 0;
        uint32_t rtMaxUs       = 0;   ///< set by the port, which owns the clock
    };

    /// Called when a voice starts and when it ends, with the pad and the layer.
    /// Fired from the render thread: whatever they do must be RT-safe.
    using NoteCb = void (*)(uint8_t pad, uint8_t layer, void* ctx);

    SamplerEngine();

    // ── IAudioNode ────────────────────────────────────────────────────────
    const char* name() const override { return "Sampler"; }
    bool isGenerator() const override { return true; }
    void onPrepare(uint32_t maxFrames, uint32_t sampleRate) override;
    void mix(float* out, uint32_t frames) override;

    // ── Setup, on the loader thread ───────────────────────────────────────
    /// The pool the voices stream through and the store they read.
    void bind(BlockPool* pool, SampleStore* store);
    void setParams(const Params& p);
    void setNoteCallbacks(NoteCb on, NoteCb off, void* ctx);

    /**
     * @brief Close the note path so the loader can retire the current kit.
     *
     * Between these two, noteOn() does nothing, so no voice can take a fresh
     * reference into a kit that is about to go. Not re-entrant: one mutator.
     */
    void beginMutation();
    void endMutation();

    /// Publish a kit. nullptr silences the engine. Call it inside a mutation
    /// window, with every voice already stopped.
    void setKit(const SamplerKit* kit);
    const SamplerKit* kit() const { return kit_.load(std::memory_order_acquire); }

    /// Level and pan of a pad, live, without a reload -- what the Pad Mixer
    /// turns. Applies to voices already sounding.
    void setPadMix(uint8_t pad, int volume, int pan);
    /// The same, already in the engine's own units: linear gain and -1..+1.
    void setPadMixGains(uint8_t pad, float gain, float balance);

    // ── Notes, from the event thread ──────────────────────────────────────
    void noteOn(uint8_t pad, uint8_t velocity);
    void noteOff(uint8_t pad);
    void allNotesOff();

    /// True while any voice of @p pad is rendering -- what lets the kit loader
    /// rebuild a pad without gating the render thread.
    bool padSounding(uint8_t pad) const;
    bool anySounding() const;

    // ── Streaming, from the BG thread ─────────────────────────────────────
    /// Refill what the sounding voices will want next, and give back the
    /// blocks of the ones that have ended. Never called on the render thread.
    void serviceIo();

    Counters counters() const;
    void     resetCounters();
    uint8_t  voicesActive() const;

private:
    enum class VoiceState : uint8_t { Free = 0, Claiming, Playing, Finished };

    struct Voice {
        std::atomic<uint8_t> state{static_cast<uint8_t>(VoiceState::Free)};
        std::atomic<uint32_t> playFrame{0};   ///< published for serviceIo()
        std::atomic<bool>     fading{false};  ///< asked to give its slot up
        std::atomic<bool>     releasing{false};
        bool                  announced = false;   ///< onNote fired from the render pass

        SampleCursor cursor;
        StreamWindow window;

        uint64_t phase = 0;
        uint64_t inc   = 0;
        uint32_t first = 0;      ///< trim start, source frames
        uint32_t last  = 0;      ///< trim end, exclusive
        bool     loop  = false;

        float gainL = 0.0f, gainR = 0.0f;
        float env = 0.0f, envStep = 0.0f, envTarget = 1.0f;

        uint8_t  pad = 0, layer = 0, choke = 0;
        uint32_t age = 0;        ///< order of starting, for the steal
        uint32_t quiet = 0;      ///< consecutive frames below silenceLevel
    };

    Voice* claimVoice();
    void   stealOldest();
    void   startFade(Voice& v);
    void   endVoice(Voice& v);
    bool   renderVoice(Voice& v, float* out, uint32_t frames);

    BlockPool*   pool_  = nullptr;
    SampleStore* store_ = nullptr;

    std::atomic<const SamplerKit*> kit_{nullptr};
    std::atomic<bool> mutating_{false};

    Voice voices_[kMaxVoices];
    std::atomic<uint32_t> ageCounter_{0};
    std::atomic<uint32_t> roundRobin_[kPads] = {};
    std::atomic<uint32_t> padVoices_[kPads] = {};   ///< one bit per voice slot

    Params params_;
    uint32_t sampleRate_ = 44100;

    NoteCb onNote_ = nullptr, offNote_ = nullptr;
    void*  cbCtx_  = nullptr;

    std::atomic<uint32_t> cNotes_{0}, cDropped_{0}, cSteals_{0};
    std::atomic<uint32_t> cStreamRefused_{0}, cUnderruns_{0}, cVoicesPeak_{0}, cSilenced_{0};
};

} // namespace crosspad

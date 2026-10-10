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
         * Silence is judged on the file, not on what the pad's gain makes of
         * it, and where it was found is kept (SampleStore::audibleEnd()): the
         * next hit of that sample stops exactly there and never streams the
         * tail at all. So the wait only matters for a sample's first hit, and
         * it is long -- at 200 ms it took a rest inside a phrase for the end
         * of the file and cut the phrase off there (DREAMSCAPE pad 4: a
         * 221 ms rest at 1.05 s, 1.8 s of the sample never played,
         * 09.10.2026).
         *
         * Only one-shots. A loop is meant to carry on, and a held note is the
         * player's to release. 0 disables it.
         */
        float   silenceMs = 1500.0f;
        /**
         * @brief Voices that may be **streaming** at once.
         *
         * A resident voice is arithmetic; a streaming one also has to be fed
         * off the card, and that is the smaller budget. Measured on a v2 with
         * the card at 4-bit/40 MHz, 2 s stereo samples: ten stream cleanly at
         * 78 % of the block, at eleven voices start ending early and at twelve
         * the underrun counter moves.
         *
         * Without this the engine happily starts a twelfth streaming voice and
         * several of them then starve -- which is heard as notes cutting out
         * at random. With it, the oldest streaming voice is faded instead:
         * one deliberate ending rather than a handful of ragged ones, and the
         * hit the player just made is the one that survives.
         *
         * Resident voices are not counted against it; `maxVoices` still caps
         * the total.
         */
        uint8_t maxStreamingVoices = 10;

        /**
         * @brief A pad cuts its own sound when it is hit again.
         *
         * True because that is what this instrument does: the prebuilt engine
         * held one player slot per pad, so a second hit restarted the pad
         * rather than layering on it, and every kit and pattern here was made
         * against that. A voice-per-note engine layers unless told otherwise,
         * which was heard at once as a pad "playing several times from the
         * beginning".
         *
         * The cut is the steal's fade, not a hard stop. Set false for a kit
         * whose pads are meant to pile up.
         */
        bool monoPerPad = true;

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
        /**
         * @brief Longest noteOn() call in **wall time** -- work and waiting both.
         *
         * It was added to tell two explanations for a late note apart: a slow
         * noteOn(), or a note thread that is not being scheduled. It does not,
         * because it is a clock read either side of the call, so a thread
         * descheduled inside it counts the same as work. That is worth knowing
         * before trusting the number.
         *
         * What it did settle, by subtraction: with the pool taken off the note
         * path entirely -- no blocks taken, no lock touched, nothing left but
         * field writes -- it still measured **98 642 us** on the board. A call
         * that does nothing cannot take 99 ms, so that is the note thread
         * waiting for its core, and the engine is not what makes a note late.
         *
         * It stays because that subtraction is worth being able to repeat, not
         * because the number means what its name suggests.
         */
        uint32_t noteOnMaxUs   = 0;
    };

    /// Microseconds, for the one thing this engine times itself. The port
    /// gives it the platform's clock; without one, noteOnMaxUs stays 0.
    using NowFn = uint32_t (*)();
    void setClock(NowFn now) { now_ = now; }

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
    /// Play @p pad. @p transpose: semitones on top of the pad's own pitch, for
    /// this note only. @p key: which hit this is -- the retrigger and the
    /// release go by it, so 16 levels of pitch (every pad playing one pad's
    /// sample) keeps a chord instead of each note cutting the one before.
    /// kOwnKey: the pad itself.
    static constexpr uint8_t kOwnKey = 0xFF;
    void noteOn(uint8_t pad, uint8_t velocity, int8_t transpose = 0, uint8_t key = kOwnKey);
    /// Release the notes of @p pad struck as @p key (kOwnKey: the pad's own).
    void noteOff(uint8_t pad, uint8_t key = kOwnKey);
    void allNotesOff();

    /**
     * @brief Silence one pad now, whatever its mode says.
     *
     * Not noteOff(): a release is only heard in Hold mode, and this is the
     * other thing an editor needs -- the pad is about to point at a different
     * file, so what is sounding on it has to go whether it is a one-shot or a
     * loop. Fades rather than cuts, for the same reason a steal does.
     */
    void stopPad(uint8_t pad);

    /// How many sounding voices are reading the card rather than RAM. The
    /// budget that binds first on a kit of long samples.
    uint8_t streamingVoices() const;

    /// What a meter should show: voices sounding, and the limit that applies.
    /// `limit` is the streaming budget once anything streams, because that is
    /// the one the player will hit.
    struct Meter {
        uint8_t active    = 0;
        uint8_t streaming = 0;
        uint8_t limit     = 0;
    };
    Meter meter() const;

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
        uint8_t  key = 0;        ///< the hit it belongs to: the pad, or what noteOn() was given
        uint32_t age = 0;        ///< order of starting, for the steal
        uint32_t quiet = 0;      ///< consecutive frames below silenceLevel
        uint32_t loudEnd = 0;    ///< one past the last frame above it, 0 = none yet
        uint16_t slotIdx = SampleStore::kNoSlot;   ///< where to say so
        /* What the streaming thread needs to open this voice's ring, left for
         * it by noteOn() rather than done there. */
        const SampleStore::Slot* slot = nullptr;
        /** Still waiting for its ring.
         *
         * **Not "this voice streams".** `serviceIo()` clears it the moment it
         * opens the window, so a voice that is reading the card has it false
         * and `window.isOpen()` true. Ask `streamsOrWillStream()` instead;
         * reading this flag as "is streaming" made the streaming budget see
         * nothing at all. */
        bool wantStream = false;
    };

    void   noteOnBody(uint8_t pad, uint8_t velocity, int8_t transpose, uint8_t key);
    Voice* claimVoice();
    void   stealOldest();
    /// The oldest voice that is reading the card. Used when the streaming
    /// budget is full and a resident voice would be the wrong one to lose.
    void   stealOldestStreaming();
    /// Whether @p v is charged to the streaming budget: still waiting for a
    /// ring, or already being fed through one. See the definition -- the
    /// `wantStream` flag alone is not this question.
    static bool streamsOrWillStream(const Voice& v);
    void   startFade(Voice& v);
    /// A one-shot has run past its sound: tell the store where it ended.
    void   learnEnd(const Voice& v);
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

    NowFn  now_ = nullptr;
    std::atomic<uint32_t> cNoteOnMaxUs_{0};
    NoteCb onNote_ = nullptr, offNote_ = nullptr;
    void*  cbCtx_  = nullptr;

    std::atomic<uint32_t> cNotes_{0}, cDropped_{0}, cSteals_{0};
    std::atomic<uint32_t> cStreamRefused_{0}, cUnderruns_{0}, cVoicesPeak_{0}, cSilenced_{0};
};

} // namespace crosspad

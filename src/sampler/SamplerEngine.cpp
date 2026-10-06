// SPDX-License-Identifier: MIT
#include "crosspad/sampler/SamplerEngine.hpp"

#include <cmath>
#include <cstddef>

namespace crosspad {

namespace {
constexpr uint64_t kOne = 1ull << 32;

/// Semitones to a speed ratio. Called when a note starts, never per frame.
float semitoneRatio(int semitones)
{
    return std::pow(2.0f, static_cast<float>(semitones) / 12.0f);
}
} // namespace

SamplerEngine::SamplerEngine() = default;

void SamplerEngine::bind(BlockPool* pool, SampleStore* store)
{
    pool_ = pool;
    store_ = store;
}

void SamplerEngine::setParams(const Params& p)
{
    params_ = p;
    if (params_.maxVoices > kMaxSounding) params_.maxVoices = kMaxSounding;
    if (params_.maxVoices == 0) params_.maxVoices = 1;
}

void SamplerEngine::setNoteCallbacks(NoteCb on, NoteCb off, void* ctx)
{
    onNote_ = on;
    offNote_ = off;
    cbCtx_ = ctx;
}

void SamplerEngine::onPrepare(uint32_t /*maxFrames*/, uint32_t sampleRate)
{
    /* Called again whenever a channel is added or the engine is set up anew,
     * so it may not assume it is the first time. Nothing here is sized by the
     * block length: voices render straight into the caller's buffer. */
    if (sampleRate) sampleRate_ = sampleRate;
}

void SamplerEngine::beginMutation()
{
    mutating_.store(true, std::memory_order_release);
}

void SamplerEngine::endMutation()
{
    mutating_.store(false, std::memory_order_release);
}

void SamplerEngine::setKit(const SamplerKit* k)
{
    kit_.store(k, std::memory_order_release);
}

void SamplerEngine::setPadMix(uint8_t pad, int volume, int pan)
{
    setPadMixGains(pad, SamplerKit::volumeToGain(volume), SamplerKit::panToBalance(pan));
}

void SamplerEngine::setPadMixGains(uint8_t pad, float gain, float balance)
{
    if (pad >= kPads) return;
    float l = 0, r = 0;
    SamplerKit::panGains(balance, l, r);
    const float g = gain;
    /* Live, on voices already sounding: the Pad Mixer's fader is turned while
     * a loop is playing and must be heard on that loop, not on the next hit. */
    for (auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) != static_cast<uint8_t>(VoiceState::Playing)) continue;
        if (v.pad != pad) continue;
        v.gainL = g * l;
        v.gainR = g * r;
    }
}

SamplerEngine::Voice* SamplerEngine::claimVoice()
{
    for (auto& v : voices_) {
        uint8_t expect = static_cast<uint8_t>(VoiceState::Free);
        if (v.state.compare_exchange_strong(expect, static_cast<uint8_t>(VoiceState::Claiming),
                                            std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return &v;
        }
    }
    return nullptr;
}

void SamplerEngine::startFade(Voice& v)
{
    v.fading.store(true, std::memory_order_release);
}

void SamplerEngine::stealOldest()
{
    Voice* oldest = nullptr;
    uint32_t best = 0xFFFFFFFFu;
    for (auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) != static_cast<uint8_t>(VoiceState::Playing)) continue;
        if (v.fading.load(std::memory_order_acquire)) continue;       // already on its way out
        if (v.age < best) { best = v.age; oldest = &v; }
    }
    if (oldest) {
        startFade(*oldest);
        cSteals_.fetch_add(1, std::memory_order_relaxed);
    }
}

uint8_t SamplerEngine::voicesActive() const
{
    uint8_t n = 0;
    for (const auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) == static_cast<uint8_t>(VoiceState::Playing)) ++n;
    }
    return n;
}

void SamplerEngine::noteOn(uint8_t pad, uint8_t velocity)
{
    const uint32_t t0 = now_ ? now_() : 0;
    noteOnBody(pad, velocity);
    if (now_) {
        const uint32_t dt = now_() - t0;
        uint32_t m = cNoteOnMaxUs_.load(std::memory_order_relaxed);
        while (dt > m && !cNoteOnMaxUs_.compare_exchange_weak(m, dt, std::memory_order_relaxed)) { }
    }
}

void SamplerEngine::noteOnBody(uint8_t pad, uint8_t velocity)
{
    if (pad >= kPads) return;
    if (mutating_.load(std::memory_order_acquire)) return;
    const SamplerKit* k = kit_.load(std::memory_order_acquire);
    if (!k || !store_ || !pool_) return;

    const KitPad& kp = k->pad[pad];
    if (kp.layerCount == 0) return;

    /* Velocity 0 is a note-off at the bridge, so anything that reaches here
     * sounds; a pad hit with no force still has to make a sound. */
    if (velocity == 0) velocity = 1;

    /* Which layer. A velocity split by default, in the order the kit lists
     * them; round-robin where the kit asks for it. */
    uint8_t layer;
    if (kp.roundRobin) {
        layer = static_cast<uint8_t>(roundRobin_[pad].fetch_add(1, std::memory_order_relaxed)
                                     % kp.layerCount);
    } else {
        layer = static_cast<uint8_t>((velocity * kp.layerCount) / 128);
        if (layer >= kp.layerCount) layer = kp.layerCount - 1;
    }

    const KitLayer& kl = kp.layers[layer];
    const SampleStore::Slot* slot = store_->slot(kl.slot);
    if (!slot) return;

    /* Choke: a hit in a group fades what the group was already playing. A cut
     * at an arbitrary phase is a click, so this is the same fade a steal uses. */
    if (kp.choke != 0) {
        for (auto& v : voices_) {
            if (v.state.load(std::memory_order_acquire) != static_cast<uint8_t>(VoiceState::Playing)) continue;
            if (v.choke == kp.choke) startFade(v);
        }
    }

    if (voicesActive() >= params_.maxVoices) stealOldest();

    Voice* vp = claimVoice();
    if (!vp) {
        cDropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Voice& v = *vp;

    const uint32_t total = slot->desc.frames;
    uint32_t first = kl.start < total ? kl.start : 0;
    uint32_t last  = (kl.end == 0 || kl.end > total) ? total : kl.end;
    if (first >= last) { first = 0; last = total; }

    /* The streamed part, if any. Opened here, off the render thread; a
     * refusal is a voice that plays its resident head and stops, which is a
     * short note rather than a glitch, and it is counted. */
    v.window.close(*pool_);
    const bool needsStream = !slot->complete && last > slot->headFrames;
    if (needsStream) {
        if (!v.window.open(*pool_, slot->path, slot->desc.channels, slot->headFrames, last)) {
            cStreamRefused_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    v.cursor.bind(slot->head, slot->headFrames, slot->desc.channels, last,
                  v.window.isOpen() ? &v.window : nullptr);

    /* One increment carries both reasons a voice does not step by exactly one
     * frame: the file's rate against the engine's, and the pad's pitch. The
     * engine this replaces could do neither, and the firmware wrote
     * transposed copies of the files to the card instead. */
    const double rate  = static_cast<double>(slot->desc.sampleRate) / static_cast<double>(sampleRate_);
    const double ratio = rate * static_cast<double>(semitoneRatio(kp.semitones));
    v.inc = static_cast<uint64_t>(ratio * static_cast<double>(kOne));
    if (v.inc == 0) v.inc = 1;

    v.phase = static_cast<uint64_t>(first) * kOne;
    v.first = first;
    v.last  = last;
    v.loop  = (kp.mode == PadMode::Loop);
    v.pad   = pad;
    v.layer = layer;
    v.choke = kp.choke;

    float pl = 0, pr = 0;
    SamplerKit::panGains(kp.pan, pl, pr);
    const float g = kp.gain * (velocity / 127.0f);
    v.gainL = g * pl;
    v.gainR = g * pr;

    v.env = 0.0f;
    v.envTarget = 1.0f;
    const float attackFrames = params_.attackMs * 0.001f * static_cast<float>(sampleRate_);
    v.envStep = attackFrames > 1.0f ? (1.0f / attackFrames) : 1.0f;

    v.quiet = 0;
    v.announced = false;
    v.fading.store(false, std::memory_order_relaxed);
    v.releasing.store(false, std::memory_order_relaxed);
    v.playFrame.store(first, std::memory_order_relaxed);
    v.age = ageCounter_.fetch_add(1, std::memory_order_relaxed);

    const size_t idx = static_cast<size_t>(&v - voices_);
    padVoices_[pad].fetch_or(1u << idx, std::memory_order_acq_rel);

    /* Published last: everything above is written before any other thread may
     * look at this slot. */
    v.state.store(static_cast<uint8_t>(VoiceState::Playing), std::memory_order_release);
    cNotes_.fetch_add(1, std::memory_order_relaxed);

    const uint32_t act = voicesActive();
    uint32_t pk = cVoicesPeak_.load(std::memory_order_relaxed);
    while (act > pk && !cVoicesPeak_.compare_exchange_weak(pk, act, std::memory_order_relaxed)) { }
}

void SamplerEngine::noteOff(uint8_t pad)
{
    if (pad >= kPads) return;
    const SamplerKit* k = kit_.load(std::memory_order_acquire);
    /* Only Hold mode listens to a release. A one-shot plays to its end and a
     * loop keeps going -- that is what the kit's playbackMode means. */
    if (k && k->pad[pad].mode != PadMode::Hold) return;
    for (auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) != static_cast<uint8_t>(VoiceState::Playing)) continue;
        if (v.pad == pad) v.releasing.store(true, std::memory_order_release);
    }
}

void SamplerEngine::allNotesOff()
{
    for (auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) == static_cast<uint8_t>(VoiceState::Playing)) {
            startFade(v);
        }
    }
}

bool SamplerEngine::padSounding(uint8_t pad) const
{
    return pad < kPads && padVoices_[pad].load(std::memory_order_acquire) != 0;
}

bool SamplerEngine::anySounding() const
{
    for (const auto& p : padVoices_) {
        if (p.load(std::memory_order_acquire) != 0) return true;
    }
    return false;
}

void SamplerEngine::endVoice(Voice& v)
{
    const size_t idx = static_cast<size_t>(&v - voices_);
    padVoices_[v.pad].fetch_and(~(1u << idx), std::memory_order_acq_rel);
    if (offNote_) offNote_(v.pad, v.layer, cbCtx_);
    /* Finished, not Free: the blocks are given back by the streaming thread.
     * Releasing them here would be the render thread reaching into the pool's
     * free list while a refill may be walking it -- the same rule that keeps
     * fclose() off this thread. */
    v.state.store(static_cast<uint8_t>(VoiceState::Finished), std::memory_order_release);
}

bool SamplerEngine::renderVoice(Voice& v, float* out, uint32_t frames)
{
    const float fadeFrames = kStealFadeMs * 0.001f * static_cast<float>(sampleRate_);
    const float fadeStep = fadeFrames > 1.0f ? (1.0f / fadeFrames) : 1.0f;
    const float relFrames = params_.releaseMs * 0.001f * static_cast<float>(sampleRate_);
    const float relStep = relFrames > 1.0f ? (1.0f / relFrames) : 1.0f;

    const bool fading = v.fading.load(std::memory_order_acquire);
    const bool releasing = v.releasing.load(std::memory_order_acquire);
    const float down = fading ? fadeStep : (releasing ? relStep : 0.0f);

    /* Only a one-shot: a loop is meant to carry on and a held note is the
     * player's to release. */
    const uint32_t quietLimit =
        (!v.loop && !releasing && params_.silenceMs > 0.0f)
            ? static_cast<uint32_t>(params_.silenceMs * 0.001f * static_cast<float>(sampleRate_))
            : 0;

    const uint64_t lastQ = static_cast<uint64_t>(v.last) * kOne;
    const uint64_t firstQ = static_cast<uint64_t>(v.first) * kOne;
    const uint64_t spanQ = lastQ > firstQ ? (lastQ - firstQ) : kOne;

    for (uint32_t i = 0; i < frames; ++i) {
        float l = 0.0f, r = 0.0f;
        if (!v.cursor.read(v.phase, l, r)) {
            /* The frame is not here. Either the sample ended -- the ordinary
             * case -- or the streamer has not delivered it, which is the one
             * thing this engine must never do quietly. */
            if ((v.phase >> 32) + 2 < static_cast<uint64_t>(v.last)) {
                cUnderruns_.fetch_add(1, std::memory_order_relaxed);
            }
            return false;
        }

        if (down > 0.0f) {
            v.env -= down;
            if (v.env <= 0.0f) return false;
        } else if (v.env < 1.0f) {
            v.env += v.envStep;
            if (v.env > 1.0f) v.env = 1.0f;
        }

        const float e = v.env * (1.0f / 32768.0f);
        const float ol = l * v.gainL * e;
        const float orr = r * v.gainR * e;
        out[i * 2 + 0] += ol;
        out[i * 2 + 1] += orr;

        /* A one-shot that has gone inaudible is stopped rather than played
         * and streamed to the end of its file. Counted on what actually
         * reaches the bus, after the gain and the envelope, so a pad turned
         * down does not keep a voice alive on a signal nobody can hear. */
        if (quietLimit) {
            const float a = (ol < 0 ? -ol : ol) > (orr < 0 ? -orr : orr)
                                ? (ol < 0 ? -ol : ol) : (orr < 0 ? -orr : orr);
            if (a < params_.silenceLevel) {
                if (++v.quiet >= quietLimit) {
                    cSilenced_.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
            } else {
                v.quiet = 0;
            }
        }

        v.phase += v.inc;
        if (v.phase >= lastQ) {
            if (!v.loop) return false;
            v.phase = firstQ + (v.phase - firstQ) % spanQ;
        }
    }
    v.playFrame.store(static_cast<uint32_t>(v.phase >> 32), std::memory_order_release);
    return true;
}

void SamplerEngine::mix(float* out, uint32_t frames)
{
    for (auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) != static_cast<uint8_t>(VoiceState::Playing)) continue;
        /* Announced from the render pass, not from noteOn(): what the Sampler
         * lights and what SMPL_LAST reports is the voice that is being heard,
         * not the one that was asked for. */
        if (!v.announced) {
            v.announced = true;
            if (onNote_) onNote_(v.pad, v.layer, cbCtx_);
        }
        if (!renderVoice(v, out, frames)) endVoice(v);
    }
}

void SamplerEngine::serviceIo()
{
    if (!pool_ || !store_) return;
    ISampleSource* src = store_->source();
    for (auto& v : voices_) {
        const uint8_t st = v.state.load(std::memory_order_acquire);
        if (st == static_cast<uint8_t>(VoiceState::Finished)) {
            /* The render thread has let this one go. Its blocks come back here
             * because this thread is allowed to touch the pool's free list
             * without being on a 1.45 ms clock. */
            v.window.close(*pool_);
            v.cursor.clear();
            v.state.store(static_cast<uint8_t>(VoiceState::Free), std::memory_order_release);
            continue;
        }
        if (st != static_cast<uint8_t>(VoiceState::Playing)) continue;
        if (!v.window.isOpen() || !src) continue;
        v.window.service(*src, v.playFrame.load(std::memory_order_acquire));
    }
}

SamplerEngine::Counters SamplerEngine::counters() const
{
    Counters c;
    c.notesStarted  = cNotes_.load(std::memory_order_relaxed);
    c.silenced      = cSilenced_.load(std::memory_order_relaxed);
    c.noteOnMaxUs   = cNoteOnMaxUs_.load(std::memory_order_relaxed);
    c.notesDropped  = cDropped_.load(std::memory_order_relaxed);
    c.steals        = cSteals_.load(std::memory_order_relaxed);
    c.streamRefused = cStreamRefused_.load(std::memory_order_relaxed);
    c.underruns     = cUnderruns_.load(std::memory_order_relaxed);
    c.voicesPeak    = cVoicesPeak_.load(std::memory_order_relaxed);
    return c;
}

void SamplerEngine::resetCounters()
{
    cNotes_ = 0; cDropped_ = 0; cSteals_ = 0;
    cStreamRefused_ = 0; cUnderruns_ = 0; cVoicesPeak_ = 0; cSilenced_ = 0;
    cNoteOnMaxUs_ = 0;
}

} // namespace crosspad

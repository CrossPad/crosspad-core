// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/sampler/SamplerEngine.hpp"
#include "crosspad/dsp/AudioKernels.hpp"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

/* Set-up, a kit's take and the reports run on a worker or a verb, never in the
 * render; the board builds this file for speed (-O2) for the render's sake.
 * GCC's `cold` builds these for size and changes nothing about what they do. */
#if defined(__GNUC__)
#define CROSSPAD_COLD __attribute__((cold))
#else
#define CROSSPAD_COLD
#endif

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

CROSSPAD_COLD void SamplerEngine::bind(BlockPool* pool, SampleStore* store)
{
    pool_ = pool;
    store_ = store;
}

CROSSPAD_COLD void SamplerEngine::setParams(const Params& p)
{
    params_ = p;
    if (params_.maxVoices > kMaxSounding) params_.maxVoices = kMaxSounding;
    if (params_.maxVoices == 0) params_.maxVoices = 1;
}

CROSSPAD_COLD void SamplerEngine::setNoteCallbacks(NoteCb on, NoteCb off, void* ctx)
{
    onNote_ = on;
    offNote_ = off;
    cbCtx_ = ctx;
}

CROSSPAD_COLD void SamplerEngine::onPrepare(uint32_t /*maxFrames*/, uint32_t sampleRate)
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

CROSSPAD_COLD void SamplerEngine::setKit(const SamplerKit* k)
{
    kit_.store(k, std::memory_order_release);
}

CROSSPAD_COLD void SamplerEngine::setBarClock(BarClockFn fn, void* ctx)
{
    barClockCtx_ = ctx;
    barClock_ = fn;
}

bool SamplerEngine::barClockRunning() const
{
    uint32_t toBar = 0;
    return barClock_ && barClock_(barClockCtx_, toBar);
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

bool SamplerEngine::stealOldest()
{
    Voice* oldest = nullptr;
    uint32_t best = 0xFFFFFFFFu;
    for (auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) != static_cast<uint8_t>(VoiceState::Playing)) continue;
        if (v.fading.load(std::memory_order_acquire)) continue;       // already on its way out
        if (v.age < best) { best = v.age; oldest = &v; }
    }
    if (!oldest) return false;
    startFade(*oldest);
    cSteals_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

/* Is this voice charged to the streaming budget?
 *
 * **Not `wantStream` alone.** That flag means "this voice still needs its
 * ring opened": `serviceIo()` clears it the moment the window is open, so a
 * voice that is actually reading the card has it *false*. Counting it alone
 * made the budget see nothing and never fire -- a host test caught that, by
 * asking for steals that never happened.
 *
 * Both halves, so the count is exact in the window between a note and the
 * streaming pass that serves it: still waiting (`wantStream`), or already
 * being fed (`window.isOpen()`).
 */
bool SamplerEngine::streamsOrWillStream(const Voice& v)
{
    return v.wantStream || v.window.isOpen();
}

void SamplerEngine::stealOldestStreaming()
{
    Voice* oldest = nullptr;
    uint32_t best = 0xFFFFFFFFu;
    for (auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) != static_cast<uint8_t>(VoiceState::Playing)) continue;
        if (v.fading.load(std::memory_order_acquire)) continue;
        if (!streamsOrWillStream(v)) continue;    // a resident voice costs the card nothing
        if (v.age < best) { best = v.age; oldest = &v; }
    }
    if (oldest) {
        startFade(*oldest);
        cSteals_.fetch_add(1, std::memory_order_relaxed);
    }
}

uint8_t SamplerEngine::streamingVoices() const
{
    uint8_t n = 0;
    for (const auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) != static_cast<uint8_t>(VoiceState::Playing)) continue;
        if (streamsOrWillStream(v)) ++n;
    }
    return n;
}

CROSSPAD_COLD SamplerEngine::Meter SamplerEngine::meter() const
{
    Meter m;
    for (const auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) != static_cast<uint8_t>(VoiceState::Playing)) continue;
        ++m.active;
        if (streamsOrWillStream(v)) ++m.streaming;
    }
    /* Which ceiling applies comes from the **kit**, not from what happens to
     * be sounding this instant.
     *
     * Deriving it from `m.streaming` looked right and was wrong: the limit
     * flipped back to the resident one the moment the last streaming voice
     * faded, so a meter would have flickered between two numbers while a
     * player held a groove. A kit holding any sample too long to be resident
     * can be asked for a streaming voice at any time, so that is its ceiling
     * for as long as it is loaded. (A host test caught this.) */
    bool anyStreams = false;
    if (store_) {
        const SampleStore::Stats st = store_->stats();
        anyStreams = st.slots > st.completeSlots;
    }
    m.limit = anyStreams ? params_.maxStreamingVoices : params_.maxVoices;
    return m;
}

uint8_t SamplerEngine::voicesActive() const
{
    uint8_t n = 0;
    for (const auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) == static_cast<uint8_t>(VoiceState::Playing)) ++n;
    }
    return n;
}

void SamplerEngine::noteOn(uint8_t pad, uint8_t velocity, int8_t transpose, uint8_t key)
{
    /* Counted before the kit is read and released after the voice is
     * published, both sequentially consistent: a loader that rewrote a pad
     * and then finds no note in flight knows every later note reads the new
     * pad (slotInUse()). */
    notesInFlight_.fetch_add(1, std::memory_order_seq_cst);
    const uint32_t t0 = now_ ? now_() : 0;
    noteOnBody(pad, velocity, transpose, key == kOwnKey ? pad : key);
    if (now_) {
        const uint32_t dt = now_() - t0;
        uint32_t m = cNoteOnMaxUs_.load(std::memory_order_relaxed);
        while (dt > m && !cNoteOnMaxUs_.compare_exchange_weak(m, dt, std::memory_order_relaxed)) { }
    }
    notesInFlight_.fetch_sub(1, std::memory_order_seq_cst);
}

void SamplerEngine::noteOnBody(uint8_t pad, uint8_t velocity, int8_t transpose, uint8_t key)
{
    if (pad >= kPads) return;
    if (mutating_.load(std::memory_order_acquire)) return;
    const SamplerKit* k = kit_.load(std::memory_order_acquire);
    if (!k || !store_ || !pool_) return;

    const KitPad& kp = k->pad[pad];
    if (kp.layerCount == 0) return;
    const bool isLoop = kp.mode == PadMode::Loop;
    if (isLoop && loopToggle(pad, key)) return;

    /* Velocity 0 is a note-off at the bridge, so anything that reaches here
     * sounds; a pad hit with no force still has to make a sound. */
    if (velocity == 0) velocity = 1;

    Plan p;
    if (!plan(kp, pickLayer(kp, pad, velocity), p)) return;
    cutFor(kp, key);
    makeRoom(p.willStream);

    Voice* vp = claimVoice();
    if (!vp) {
        cDropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    fill(*vp, kp, p, pad, transpose, key);
    if (isLoop && barClockRunning()) {
        vp->queued.store(true, std::memory_order_relaxed);
        cLoopsQueued_.fetch_add(1, std::memory_order_relaxed);
    }
    start(*vp, kp, velocity);
}

bool SamplerEngine::loopToggle(uint8_t pad, uint8_t key)
{
    for (auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) != static_cast<uint8_t>(VoiceState::Playing)) continue;
        if (!v.loop || v.pad != pad || v.key != key) continue;
        if (v.fading.load(std::memory_order_acquire)) continue;
        if (v.queued.load(std::memory_order_acquire)) {
            startFade(v);
        } else if (v.muted.load(std::memory_order_acquire)) {
            v.muted.store(false, std::memory_order_release);
        } else if (barClockRunning()) {
            v.muted.store(true, std::memory_order_release);
        } else {
            startFade(v);
        }
        return true;
    }
    return false;
}

SamplerEngine::LoopState SamplerEngine::loopState(uint8_t pad, uint8_t key) const
{
    if (pad >= kPads) return LoopState::Idle;
    if (key == kOwnKey) key = pad;
    for (const auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) != static_cast<uint8_t>(VoiceState::Playing)) continue;
        if (!v.loop || v.pad != pad || v.key != key) continue;
        if (v.fading.load(std::memory_order_acquire)) continue;
        if (v.queued.load(std::memory_order_acquire)) return LoopState::Queued;
        return v.muted.load(std::memory_order_acquire) ? LoopState::Muted : LoopState::Playing;
    }
    return LoopState::Idle;
}

uint8_t SamplerEngine::pickLayer(const KitPad& kp, uint8_t pad, uint8_t velocity)
{
    /* A velocity split by default, in the order the kit lists them;
     * round-robin where the kit asks for it. */
    if (kp.roundRobin) {
        return static_cast<uint8_t>(roundRobin_[pad].fetch_add(1, std::memory_order_relaxed) % kp.layerCount);
    }
    const uint8_t layer = static_cast<uint8_t>((velocity * kp.layerCount) / 128);
    return layer >= kp.layerCount ? static_cast<uint8_t>(kp.layerCount - 1) : layer;
}

bool SamplerEngine::plan(const KitPad& kp, uint8_t layer, Plan& p) const
{
    const KitLayer& kl = kp.layers[layer];
    const SampleStore::Slot* slot = store_->slot(kl.slot);
    if (!slot) return false;

    /* The trim, before anything is claimed: whether this note will have to be
     * fed off the card decides which budget it is charged to, and that has to
     * be known before a voice is taken. Nothing here depends on the voice. */
    const uint32_t total = slot->desc.frames;
    uint32_t first = kl.start < total ? kl.start : 0;
    uint32_t last  = (kl.end == 0 || kl.end > total) ? total : kl.end;
    if (first >= last) { first = 0; last = total; }
    /* The store began the slot's head on this very frame (trimStart(), the
     * same rule): a note starts in its head, never before it. */
    if (first < slot->headStart) first = slot->headStart;
    /* A one-shot ends where its sound does, once a voice has found where that
     * is -- and a hit whose sound fits in the head then streams nothing. */
    const uint32_t heard = store_->audibleEnd(kl.slot);
    if (kp.mode != PadMode::Loop && heard > first && heard < last) last = heard;

    p.slot = slot;
    p.slotIdx = kl.slot;
    p.layer = layer;
    p.first = first;
    p.last = last;
    p.willStream = !slot->complete && last > slot->headStart + slot->headFrames;
    return true;
}

void SamplerEngine::cutFor(const KitPad& kp, uint8_t key)
{
    /* A pad retriggers itself, and chokes its group.
     *
     * The engine this replaces had one player slot per pad, so hitting a pad
     * again restarted *that* pad -- which is what every kit, every pattern
     * and every player on this device was built against. Allocating a voice
     * per note instead meant four fast hits left four copies of the sample
     * sounding from four different places, reported as "pressing a pad plays
     * it several times from the beginning instead of starting it again".
     * Measured before the fix: one hit one voice, four hits four voices,
     * nothing cut.
     *
     * Both cuts are the fade a steal uses: a cut at an arbitrary phase is a
     * click. `monoPerPad` turns the first off for a kit that wants its pads
     * to layer. */
    for (auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) != static_cast<uint8_t>(VoiceState::Playing)) continue;
        const bool sameChoke = kp.choke != 0 && v.choke == kp.choke;
        /* By the hit, not the pad: one pad's sample struck from two pads at
         * two pitches is two notes. A plain hit's key is its pad. */
        const bool samePad   = params_.monoPerPad && v.key == key;
        if (sameChoke || samePad) startFade(v);
    }
}

void SamplerEngine::makeRoom(bool willStream)
{
    /* Two ceilings, and the one that binds depends on the note.
     *
     * A resident voice is arithmetic; a streaming one also has to be fed off
     * the card, and the card gives out first. Without the second ceiling the
     * engine starts a twelfth streaming voice and several then starve, which
     * is heard as notes cutting out at random. Fading the oldest *streaming*
     * voice instead is one deliberate ending rather than a handful of ragged
     * ones, and it leaves the resident voices -- which cost the card nothing
     * -- alone. */
    if (willStream && streamingVoices() >= params_.maxStreamingVoices) {
        stealOldestStreaming();
    }
    /* Over the render's budget a note may not add a voice either: it takes
     * the oldest one's place (setOverloaded()). */
    const uint8_t active = voicesActive();
    const bool shed = overloaded_.load(std::memory_order_relaxed) && active > 0 && active < params_.maxVoices;
    if ((active >= params_.maxVoices || shed) && stealOldest() && shed) {
        cSheds_.fetch_add(1, std::memory_order_relaxed);
    }
}

void SamplerEngine::fill(Voice& v, const KitPad& kp, const Plan& p, uint8_t pad, int8_t transpose, uint8_t key)
{
    const SampleStore::Slot* slot = p.slot;
    const uint32_t first = p.first, last = p.last;

    /* The streamed part, if any, is left to the streaming thread.
     *
     * It used to be opened here, which meant sixteen trips through a pool
     * shared with the render and the streamer on the one thread that must not
     * do any work -- the thread a player is waiting on. The resident head is
     * 0.74 s and the streaming pass runs every millisecond, so the ring is
     * always there long before the voice reaches it; there is no reason for
     * the note to pay for it. */
    v.slot = slot;
    v.wantStream = p.willStream;
    v.cursor.bind(slot->head, slot->headStart, slot->headFrames, slot->desc.channels, last, &v.window);
    /* What StreamWindow::open() will make a lap of, for the same voice. */
    const uint32_t headEnd = slot->headStart + slot->headFrames;
    const uint32_t lapFirst = first > headEnd ? first : headEnd;
    v.cursor.setLoop(kp.mode == PadMode::Loop && p.willStream && last > lapFirst ? last - lapFirst : 0);

    /* One increment carries both reasons a voice does not step by exactly one
     * frame: the file's rate against the engine's, and the pad's pitch. The
     * engine this replaces could do neither, and the firmware wrote
     * transposed copies of the files to the card instead. */
    const double rate  = static_cast<double>(slot->desc.sampleRate) / static_cast<double>(sampleRate_);
    const double ratio = rate * static_cast<double>(semitoneRatio(kp.semitones + transpose));
    v.inc = static_cast<uint64_t>(ratio * static_cast<double>(kOne));
    if (v.inc == 0) v.inc = 1;

    v.phase = static_cast<uint64_t>(first) * kOne;
    v.first = first;
    v.last  = last;
    v.loop  = (kp.mode == PadMode::Loop);
    v.pad   = pad;
    v.key   = key;
    v.transpose = transpose;
    v.layer = p.layer;
    v.choke = kp.choke;

    v.env = 0.0f;
    v.envTarget = 1.0f;
    const float attackMs = kp.attackMs >= 0.0f ? kp.attackMs : params_.attackMs;
    const float attackFrames = attackMs * 0.001f * static_cast<float>(sampleRate_);
    v.envStep = attackFrames > 1.0f ? (1.0f / attackFrames) : 1.0f;

    v.quiet = 0;
    v.loudEnd = 0;
    v.slotIdx = p.slotIdx;
    v.announced = false;
    v.fading.store(false, std::memory_order_relaxed);
    v.releasing.store(false, std::memory_order_relaxed);
    v.queued.store(false, std::memory_order_relaxed);
    v.muted.store(false, std::memory_order_relaxed);
    v.muteGain = 1.0f;
    v.playFrame.store(first, std::memory_order_relaxed);
}

void SamplerEngine::start(Voice& v, const KitPad& kp, uint8_t velocity)
{
    float pl = 0, pr = 0;
    SamplerKit::panGains(kp.pan, pl, pr);
    const float g = kp.gain * (velocity / 127.0f);
    v.gainL = g * pl;
    v.gainR = g * pr;
    v.age = ageCounter_.fetch_add(1, std::memory_order_relaxed);

    const size_t idx = static_cast<size_t>(&v - voices_);
    padVoices_[v.pad].fetch_or(1u << idx, std::memory_order_acq_rel);

    /* Published last: everything above is written before any other thread may
     * look at this slot. */
    v.state.store(static_cast<uint8_t>(VoiceState::Playing), std::memory_order_release);
    cNotes_.fetch_add(1, std::memory_order_relaxed);

    const uint32_t act = voicesActive();
    uint32_t pk = cVoicesPeak_.load(std::memory_order_relaxed);
    while (act > pk && !cVoicesPeak_.compare_exchange_weak(pk, act, std::memory_order_relaxed)) { }
}

// ── A hit before its velocity ───────────────────────────────────────────────

SamplerEngine::Voice* SamplerEngine::armedFor(uint8_t touch)
{
    for (auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) == static_cast<uint8_t>(VoiceState::Armed) &&
            v.touch == touch) {
            return &v;
        }
    }
    return nullptr;
}

bool SamplerEngine::dropArmed(Voice& v)
{
    /* Finished, not Free: the voice's cursor and slot are the streaming
     * thread's to clear, as for any voice that has ended. It never sounded,
     * so there is nothing to announce and no pad bit to clear. */
    uint8_t expect = static_cast<uint8_t>(VoiceState::Armed);
    return v.state.compare_exchange_strong(expect, static_cast<uint8_t>(VoiceState::Finished),
                                           std::memory_order_acq_rel, std::memory_order_relaxed);
}

bool SamplerEngine::arm(uint8_t touch, uint8_t pad, int8_t transpose, uint8_t key)
{
    if (touch >= kPads || pad >= kPads) return false;
    disarm(touch);
    /* Counted like a note-on: the voice takes a reference into the kit
     * (slotInUse()). */
    notesInFlight_.fetch_add(1, std::memory_order_seq_cst);
    const bool ok = armBody(touch, pad, transpose, key == kOwnKey ? pad : key);
    notesInFlight_.fetch_sub(1, std::memory_order_seq_cst);
    return ok;
}

bool SamplerEngine::armBody(uint8_t touch, uint8_t pad, int8_t transpose, uint8_t key)
{
    if (mutating_.load(std::memory_order_acquire)) return false;
    const SamplerKit* k = kit_.load(std::memory_order_acquire);
    if (!k || !store_ || !pool_) return false;
    const KitPad& kp = k->pad[pad];
    if (kp.layerCount == 0) return false;

    /* The layer the hardest hit would take: one layer at every velocity on
     * nearly every pad, and fire() takes another if the velocity says so. */
    Plan p;
    if (!plan(kp, pickLayer(kp, pad, 127), p)) return false;
    Voice* vp = claimVoice();
    if (!vp) return false;
    Voice& v = *vp;
    fill(v, kp, p, pad, transpose, key);
    v.touch = touch;
    v.armUs = now_ ? now_() : 0;
    v.state.store(static_cast<uint8_t>(VoiceState::Armed), std::memory_order_release);
    cArmed_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool SamplerEngine::fire(uint8_t touch, uint8_t pad, uint8_t velocity, int8_t transpose, uint8_t key)
{
    if (touch >= kPads) return false;
    Voice* vp = armedFor(touch);
    if (!vp) return false;
    /* Taken from Armed, so a disarm() or the timeout that comes now finds
     * nothing to end. Claiming is what slotInUse() and the render already
     * read as "being filled". */
    uint8_t expect = static_cast<uint8_t>(VoiceState::Armed);
    if (!vp->state.compare_exchange_strong(expect, static_cast<uint8_t>(VoiceState::Claiming),
                                           std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return false;
    }
    notesInFlight_.fetch_add(1, std::memory_order_seq_cst);
    const uint32_t t0 = now_ ? now_() : 0;
    const bool ok = fireBody(*vp, pad, velocity, transpose, key == kOwnKey ? pad : key);
    if (!ok) {
        vp->state.store(static_cast<uint8_t>(VoiceState::Finished), std::memory_order_release);
        cArmCancelled_.fetch_add(1, std::memory_order_relaxed);
    } else if (now_) {
        const uint32_t dt = now_() - t0;
        uint32_t m = cNoteOnMaxUs_.load(std::memory_order_relaxed);
        while (dt > m && !cNoteOnMaxUs_.compare_exchange_weak(m, dt, std::memory_order_relaxed)) { }
    }
    notesInFlight_.fetch_sub(1, std::memory_order_seq_cst);
    return ok;
}

bool SamplerEngine::fireBody(Voice& v, uint8_t pad, uint8_t velocity, int8_t transpose, uint8_t key)
{
    if (mutating_.load(std::memory_order_acquire)) return false;
    /* Not the hit that was armed: the mode moved under the finger. */
    if (v.pad != pad || v.key != key || v.transpose != transpose) return false;
    const SamplerKit* k = kit_.load(std::memory_order_acquire);
    if (!k) return false;
    const KitPad& kp = k->pad[pad];
    if (kp.layerCount == 0) return false;
    if (velocity == 0) velocity = 1;

    /* The layer, again where it can differ: a pad split by velocity, or a
     * pad whose sample was swapped while the finger was on it. Round-robin
     * keeps the one the arm moved on to. */
    const uint8_t layer = kp.roundRobin ? v.layer : pickLayer(kp, pad, velocity);
    if (layer >= kp.layerCount) return false;
    if (layer != v.layer || kp.layers[layer].slot != v.slotIdx) {
        Plan p;
        if (!plan(kp, layer, p)) return false;
        fill(v, kp, p, pad, transpose, key);
    }
    /* A Loop pad's hit is a toggle: on a loop that is already there the
     * armed voice is not needed, and the hit has been dealt with. */
    const bool isLoop = kp.mode == PadMode::Loop;
    if (isLoop && loopToggle(pad, key)) {
        v.state.store(static_cast<uint8_t>(VoiceState::Finished), std::memory_order_release);
        return true;
    }
    cutFor(kp, key);
    makeRoom(v.wantStream);
    if (isLoop && barClockRunning()) {
        v.queued.store(true, std::memory_order_relaxed);
        cLoopsQueued_.fetch_add(1, std::memory_order_relaxed);
    }
    start(v, kp, velocity);
    cArmFired_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void SamplerEngine::disarm(uint8_t touch)
{
    if (touch >= kPads) return;
    Voice* v = armedFor(touch);
    if (v && dropArmed(*v)) cArmCancelled_.fetch_add(1, std::memory_order_relaxed);
}

uint8_t SamplerEngine::armedVoices() const
{
    uint8_t n = 0;
    for (const auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) == static_cast<uint8_t>(VoiceState::Armed)) ++n;
    }
    return n;
}

void SamplerEngine::noteOff(uint8_t pad, uint8_t key)
{
    if (pad >= kPads) return;
    if (key == kOwnKey) key = pad;
    const SamplerKit* k = kit_.load(std::memory_order_acquire);
    /* Only Hold mode listens to a release. A one-shot plays to its end and a
     * loop keeps going -- that is what the kit's playbackMode means. */
    if (k && k->pad[pad].mode != PadMode::Hold) return;
    for (auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) != static_cast<uint8_t>(VoiceState::Playing)) continue;
        if (v.pad == pad && v.key == key) v.releasing.store(true, std::memory_order_release);
    }
}

void SamplerEngine::stopPad(uint8_t pad)
{
    if (pad >= kPads) return;
    for (auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) != static_cast<uint8_t>(VoiceState::Playing)) continue;
        if (v.pad == pad) startFade(v);
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

bool SamplerEngine::slotInUse(uint16_t slot) const
{
    /* The caller's rewrite of the pads is ordered before the count is read,
     * against noteOn()'s increment before it reads one: either the note sees
     * the new pad, or this sees the note. */
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (notesInFlight_.load(std::memory_order_seq_cst) != 0) return true;

    if (const SamplerKit* k = kit_.load(std::memory_order_acquire)) {
        for (const auto& kp : k->pad) {
            for (uint8_t i = 0; i < kp.layerCount && i < KitPad::kMaxLayers; ++i) {
                if (kp.layers[i].slot == slot) return true;
            }
        }
    }
    /* Finished counts: the render has let go, but until the streaming thread
     * frees the voice its window may still be in the middle of a refill that
     * reads the slot's path. */
    for (const auto& v : voices_) {
        const uint8_t st = v.state.load(std::memory_order_acquire);
        if (st == static_cast<uint8_t>(VoiceState::Free)) continue;
        if (st == static_cast<uint8_t>(VoiceState::Claiming)) return true;
        if (v.slotIdx == slot) return true;
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

void SamplerEngine::learnEnd(const Voice& v)
{
    if (v.loudEnd == 0 || store_->audibleEnd(v.slotIdx) != 0) return;
    store_->learnAudibleEnd(v.slotIdx, v.loudEnd);
}

namespace {

/* A run of frames at full level -- the envelope neither rising nor falling,
 * which is nearly every frame of a hit after its first half millisecond --
 * as a plain walk: no envelope test, no channel test, everything it reads in
 * registers. The same sums and the same loudness bookkeeping as the frame
 * at a time path. Returns the frames played: all @p n, or the index of the
 * frame on which the quiet stopped the voice (that frame was played).
 *
 * Kept out of line so it gets the registers to itself: inlined into
 * renderVoice() the gains and the loudness threshold were spilled and read
 * back from the stack every frame. */
#if defined(__GNUC__)
#define CP_OWN_REGISTERS __attribute__((noinline))
#else
#define CP_OWN_REGISTERS
#endif

template <int Ch>
CP_OWN_REGISTERS uint32_t playLevel(const int16_t* p, uint32_t n, float* o, uint32_t at,
                                    float gainL, float gainR, float e, float loudLevel,
                                    uint32_t quietLimit, uint32_t& quietIo, uint32_t& loudEndIo)
{
    uint32_t quiet = quietIo, loudEnd = loudEndIo, k = 0;
    for (; k < n; ++k, p += Ch, o += 2) {
        const float l = static_cast<float>(p[0]);
        const float r = Ch == 2 ? static_cast<float>(p[1]) : l;
        o[0] += l * gainL * e;
        o[1] += r * gainR * e;
        if (dsp::maxOf(std::fabs(l), std::fabs(r)) >= loudLevel) {
            loudEnd = at + k + 1;
            quiet = 0;
        } else if (quietLimit && ++quiet >= quietLimit) {
            break;
        }
    }
    quietIo = quiet;
    loudEndIo = loudEnd;
    return k;
}

} // namespace

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

    const float loudLevel = params_.silenceLevel * 32768.0f;   // in the file's own units

    const float muteFrames = params_.muteMs * 0.001f * static_cast<float>(sampleRate_);
    const float muteStep = muteFrames > 1.0f ? (1.0f / muteFrames) : 1.0f;
    const float muteTarget = v.muted.load(std::memory_order_acquire) ? 0.0f : 1.0f;
    float mute = v.muteGain;

    const uint64_t lastQ = static_cast<uint64_t>(v.last) * kOne;
    const uint64_t firstQ = static_cast<uint64_t>(v.first) * kOne;
    const uint64_t spanQ = lastQ > firstQ ? (lastQ - firstQ) : kOne;

    /* A one-shot whose trim ends inside its file -- a slice of one WAV --
     * fades out from tailFrom (the file's frames) to its end: stopped there
     * mid-wave it clicked. The Slicer used to write that fade into a file of
     * each slice's own. */
    uint32_t tailFrom = UINT32_MAX;
    float tailStep = 0.0f;
    if (!v.loop && v.slot && v.last < v.slot->desc.frames) {
        const float outFrames = kTrimFadeMs * 0.001f * static_cast<float>(sampleRate_);
        const uint32_t fileFrames =
            static_cast<uint32_t>(static_cast<double>(outFrames) * static_cast<double>(v.inc) / static_cast<double>(kOne));
        tailFrom = v.last > v.first + fileFrames ? v.last - fileFrames : v.first;
        tailStep = outFrames > 1.0f ? (1.0f / outFrames) : 1.0f;
    }

    /* A voice stepping exactly one frame a frame needs no interpolation: the
     * file is at the engine's rate and the pad has no pitch on it, which is
     * most of a kit. Decided once a block, not once a frame. */
    const bool unitRate = (v.inc == kOne) && ((v.phase & (kOne - 1)) == 0);

    /* What the frames change lives in locals for the block and goes back
     * into the voice once. In the voice, every sum into `out` -- a float
     * store -- might have changed the voice's own floats as far as the
     * compiler knew, so each frame reloaded and stored them; together with
     * a cursor asked frame by frame, that was most of a voice's 60 us. */
    float env = v.env;
    const float envStep = v.envStep, gainL = v.gainL, gainR = v.gainR;
    uint32_t quiet = v.quiet, loudEnd = v.loudEnd;
    uint64_t phase = v.phase;
    bool silenced = false;
    auto settle = [&]() {
        v.muteGain = mute;
        v.env = env;
        v.quiet = quiet;
        v.loudEnd = loudEnd;
        v.phase = phase;
    };

    /* One frame after it has been read: envelope, into the bus, loudness --
     * the frame-by-frame render's arithmetic in its order, which the host
     * tests hold to the bit. False: the voice ends on this frame (`silenced`
     * says whether the quiet did it). */
    auto play = [&](float l, float r, uint32_t at, float* o) -> bool {
        if (down > 0.0f) {
            env -= down;
            if (env <= 0.0f) return false;
        } else if (at >= tailFrom) {
            env -= tailStep;
            if (env <= 0.0f) return false;
        } else if (env < 1.0f) {
            env += envStep;
            if (env > 1.0f) env = 1.0f;
        }
        if (mute < muteTarget) {
            mute += muteStep;
            if (mute > muteTarget) mute = muteTarget;
        } else if (mute > muteTarget) {
            mute -= muteStep;
            if (mute < muteTarget) mute = muteTarget;
        }
        const float e = env * (1.0f / 32768.0f);
        o[0] += l * gainL * e * mute;
        o[1] += r * gainR * e * mute;
        const float a = (l < 0 ? -l : l) > (r < 0 ? -r : r) ? (l < 0 ? -l : l) : (r < 0 ? -r : r);
        if (a >= loudLevel) {
            loudEnd = at + 1;
            quiet = 0;
        } else if (quietLimit && ++quiet >= quietLimit) {
            silenced = true;
            return false;
        }
        return true;
    };
    auto stopped = [&]() {
        settle();
        if (silenced) {
            learnEnd(v);
            cSilenced_.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    };
    /* Past the trim's end: a one-shot is over, a loop goes round. */
    auto atEnd = [&]() {
        if (phase < lastQ) return true;
        if (!v.loop) {
            settle();
            if (v.last == v.slot->desc.frames) learnEnd(v);
            return false;
        }
        phase = firstQ + (phase - firstQ) % spanQ;
        v.cursor.nextLap();
        return true;
    };

    const uint8_t ch = v.cursor.channels();
    /* Counted once a block, not once a frame: an atomic a frame would cost
     * what it measures. */
    struct PitchedTally {
        std::atomic<uint32_t>& run;
        std::atomic<uint32_t>& slow;
        uint32_t r = 0, s = 0;
        ~PitchedTally() {
            if (r) run.fetch_add(r, std::memory_order_relaxed);
            if (s) slow.fetch_add(s, std::memory_order_relaxed);
        }
    } tally{cPitchedRun_, cPitchedSlow_};
    uint32_t i = 0;
    while (i < frames) {
        const uint32_t at = static_cast<uint32_t>(phase >> 32);
        const int16_t* p = nullptr;
        uint32_t n = unitRate ? v.cursor.span(at, frames - i, p) : 0;
        if (n) {
            /* A pointer walk over the run, up to the frame whose step reaches
             * the trim's end -- only that one can end the voice by its phase.
             * A run stops where the trim's fade starts, then at the end. */
            const uint32_t stopAt = at < tailFrom && tailFrom < v.last ? tailFrom : v.last;
            const uint32_t toEnd = at < stopAt ? stopAt - at : 1;
            if (n > toEnd) n = toEnd;
            float* o = out + static_cast<size_t>(i) * 2;
            if (down == 0.0f && env == 1.0f && mute == muteTarget && at < tailFrom) {
                const float e = env * (1.0f / 32768.0f);
                const float gl = gainL * mute, gr = gainR * mute;
                const uint32_t done =
                    ch == 2 ? playLevel<2>(p, n, o, at, gl, gr, e, loudLevel, quietLimit, quiet, loudEnd)
                            : playLevel<1>(p, n, o, at, gl, gr, e, loudLevel, quietLimit, quiet, loudEnd);
                if (done < n) {
                    silenced = true;
                    phase = static_cast<uint64_t>(at + done) * kOne;
                    return stopped();
                }
            } else {
                for (uint32_t k = 0; k < n; ++k, p += ch, o += 2) {
                    const float l = static_cast<float>(p[0]);
                    const float r = ch == 2 ? static_cast<float>(p[1]) : l;
                    if (!play(l, r, at + k, o)) {
                        phase = static_cast<uint64_t>(at + k) * kOne;
                        return stopped();
                    }
                }
            }
            i += n;
            phase += static_cast<uint64_t>(n) * kOne;
            if (!atEnd()) return false;
            continue;
        }

        /* Pitched, in its steady part -- no fade, no release, not in the
         * trim's tail -- a run of interpolated frames off one buffer, with
         * the arithmetic of play() for that case. Frame by frame through
         * read() and play() a pitched voice was ~170 us a block, three times
         * an unpitched one (a Slice kit's pads, measured on the board). */
        if (!unitRate && down == 0.0f && env == 1.0f && mute == muteTarget && at < tailFrom) {
            const uint32_t stopAt = tailFrom < v.last ? tailFrom : v.last;
            const float e = env * (1.0f / 32768.0f);
            const float gl = gainL * mute, gr = gainR * mute;
            float* o = out + static_cast<size_t>(i) * 2;
            const uint32_t n = v.cursor.readRun(phase, v.inc, stopAt, frames - i,
                                                [&](float l, float r, uint32_t f) {
                o[0] += l * gl * e;
                o[1] += r * gr * e;
                o += 2;
                const float a = (l < 0 ? -l : l) > (r < 0 ? -r : r) ? (l < 0 ? -l : l) : (r < 0 ? -r : r);
                if (a >= loudLevel) {
                    loudEnd = f + 1;
                    quiet = 0;
                } else if (quietLimit && ++quiet >= quietLimit) {
                    silenced = true;
                    return false;
                }
                return true;
            });
            tally.r += n;
            if (silenced) return stopped();
            if (n) {
                i += n;
                if (!atEnd()) return false;
                continue;
            }
        }

        float l = 0.0f, r = 0.0f;
        if (!unitRate) ++tally.s;
        const bool got = unitRate ? v.cursor.sample(at, l, r) : v.cursor.read(phase, l, r);
        if (!got) {
            /* The frame is not here. Either the sample ended -- the ordinary
             * case -- or the streamer has not delivered it, which is the one
             * thing this engine must never do quietly. */
            if ((phase >> 32) + 2 < static_cast<uint64_t>(v.last)) {
                cUnderruns_.fetch_add(1, std::memory_order_relaxed);
            }
            settle();
            return false;
        }
        if (!play(l, r, at, out + static_cast<size_t>(i) * 2)) return stopped();
        ++i;
        phase += v.inc;
        if (!atEnd()) return false;
    }
    settle();
    v.playFrame.store(v.cursor.streamFrame(static_cast<uint32_t>(phase >> 32)), std::memory_order_release);
    return true;
}

uint32_t SamplerEngine::streamLead() const
{
    uint32_t lead = UINT32_MAX;
    for (const auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) != static_cast<uint8_t>(VoiceState::Playing)) continue;
        const uint32_t at = v.playFrame.load(std::memory_order_acquire);
        uint32_t upTo = 0;
        if (v.window.isOpen()) {
            upTo = v.window.validUpTo();
        } else if (v.wantStream && v.slot) {
            upTo = v.slot->headStart + v.slot->headFrames;   // the head, until its ring opens
        } else {
            continue;                                         // resident: nothing to wait for
        }
        if (upTo >= v.last) continue;                         // delivered to its end
        lead = std::min(lead, upTo > at ? upTo - at : 0u);
    }
    return lead;
}

void SamplerEngine::mix(float* out, uint32_t frames)
{
    uint32_t toBar = 0;
    const bool clockRunning = barClock_ && barClock_(barClockCtx_, toBar);
    /* The clock stopping ends every loop: a launched loop belongs to the
     * groove it was quantised to. */
    if (barWasRunning_ && !clockRunning) {
        for (auto& v : voices_) {
            if (v.state.load(std::memory_order_acquire) == static_cast<uint8_t>(VoiceState::Playing) && v.loop) {
                startFade(v);
            }
        }
    }
    barWasRunning_ = clockRunning;

    for (auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) != static_cast<uint8_t>(VoiceState::Playing)) continue;
        /* A launch waiting for the bar line: not heard, not advanced, and
         * started on the frame the line falls on. Cancelled before it was
         * heard, it simply ends. */
        uint32_t at = 0;
        if (v.queued.load(std::memory_order_acquire)) {
            if (v.fading.load(std::memory_order_acquire)) { endVoice(v); continue; }
            at = clockRunning ? toBar : 0;
            if (at >= frames) continue;
            v.queued.store(false, std::memory_order_release);
            cLoopsLaunched_.fetch_add(1, std::memory_order_relaxed);
        }
        /* Announced from the render pass, not from noteOn(): what the Sampler
         * lights and what SMPL_LAST reports is the voice that is being heard,
         * not the one that was asked for. */
        if (!v.announced) {
            v.announced = true;
            if (onNote_) onNote_(v.pad, v.layer, cbCtx_);
        }
        if (!renderVoice(v, out + static_cast<size_t>(at) * 2, frames - at)) endVoice(v);
    }
}

// ── Audition ────────────────────────────────────────────────────────────────

bool SamplerEngine::previewStart(const char* path, uint32_t first, uint32_t last,
                                 float gain, float balance)
{
    if (!path || !path[0] || !pool_ || !pSrc_) return false;
    if (std::strlen(path) >= SampleStore::kMaxPath) return false;
    /* The loader is retiring the pool's pages: a request now would open a
     * ring in memory that is on its way out. */
    if (mutating_.load(std::memory_order_acquire)) return false;

    /* Odd while the record is being written; the streaming thread copies it
     * only when the count was even and did not move. */
    const uint32_t seq = pSeq_.load(std::memory_order_relaxed);
    pSeq_.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    std::snprintf(pReq_.path, sizeof(pReq_.path), "%s", path);
    pReq_.first = first;
    pReq_.last = last;
    pReq_.gain = gain;
    pReq_.balance = balance;
    pSeq_.store(seq + 2, std::memory_order_release);
    pReqGen_.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

void SamplerEngine::previewStop()
{
    /* Names the newest request: the voice of that generation fades, and if it
     * has not started yet it never will. */
    pStopGen_.store(pReqGen_.load(std::memory_order_acquire), std::memory_order_release);
}

bool SamplerEngine::previewBusy() const
{
    if (pState_.load(std::memory_order_acquire) != static_cast<uint8_t>(PreviewState::Idle)) return true;
    const uint32_t req = pReqGen_.load(std::memory_order_acquire);
    return req != pDoneGen_.load(std::memory_order_acquire) &&
           req != pStopGen_.load(std::memory_order_acquire);
}

void SamplerEngine::mixPreview(float* out, uint32_t frames)
{
    if (pState_.load(std::memory_order_acquire) != static_cast<uint8_t>(PreviewState::Playing)) return;

    const bool fading = pFade_.load(std::memory_order_acquire);
    const float fadeFrames = kStealFadeMs * 0.001f * static_cast<float>(sampleRate_);
    const float fadeStep = fadeFrames > 1.0f ? (1.0f / fadeFrames) : 1.0f;
    const uint64_t lastQ = static_cast<uint64_t>(pLast_) * kOne;
    const bool unitRate = (pInc_ == kOne) && ((pPhase_ & (kOne - 1)) == 0);

    for (uint32_t i = 0; i < frames; ++i) {
        float l = 0.0f, r = 0.0f;
        const bool got = unitRate
                             ? pCursor_.sample(static_cast<uint32_t>(pPhase_ >> 32), l, r)
                             : pCursor_.read(pPhase_, l, r);
        if (!got) {
            /* Not delivered yet is a gap, not the end: an audition is the
             * whole file off the card, and a card busy with a kit load falls
             * behind it now and then. It holds its place and goes on when
             * the ring catches up -- for kPreviewGapMaxMs; a card that was
             * pulled never catches up. A voice asked to fade has nothing to
             * fade in a gap, so it ends there and then. */
            if (!fading && (pPhase_ >> 32) + 2 < static_cast<uint64_t>(pLast_)) {
                pGap_ += frames - i;
                if (pGap_ < kPreviewGapMaxMs * (sampleRate_ / 1000)) break;
            }
            finishPreview();
            return;
        }
        pGap_ = 0;
        if (fading) {
            pEnv_ -= fadeStep;
            if (pEnv_ <= 0.0f) {
                finishPreview();
                return;
            }
        } else if (pEnv_ < 1.0f) {
            pEnv_ += pEnvStep_;
            if (pEnv_ > 1.0f) pEnv_ = 1.0f;
        }
        const float e = pEnv_ * (1.0f / 32768.0f);
        out[i * 2 + 0] += l * pGainL_ * e;
        out[i * 2 + 1] += r * pGainR_ * e;
        pPhase_ += pInc_;
        if (pPhase_ >= lastQ) {
            finishPreview();
            return;
        }
    }
    pPlayFrame_.store(static_cast<uint32_t>(pPhase_ >> 32), std::memory_order_release);
}

void SamplerEngine::finishPreview()
{
    /* Playing -> Finished only: a voice a teardown has abandoned meanwhile is
     * not the render's to mark any more. */
    uint8_t expect = static_cast<uint8_t>(PreviewState::Playing);
    pState_.compare_exchange_strong(expect, static_cast<uint8_t>(PreviewState::Finished),
                                    std::memory_order_acq_rel, std::memory_order_relaxed);
}

CROSSPAD_COLD bool SamplerEngine::previewAbandon()
{
    /* Not while a streaming pass is in the audition: the caller tries again
     * after letting it run, rather than spinning here against a thread that
     * may be below it on the same core. */
    if (pSvc_.exchange(true, std::memory_order_acquire)) return false;
    if (pState_.load(std::memory_order_acquire) != static_cast<uint8_t>(PreviewState::Idle)) {
        closePreview();
    }
    pDoneGen_.store(pReqGen_.load(std::memory_order_acquire), std::memory_order_release);
    pSvc_.store(false, std::memory_order_release);
    return true;
}

void SamplerEngine::closePreview()
{
    pWindow_.close(*pool_);
    pCursor_.clear();
    pState_.store(static_cast<uint8_t>(PreviewState::Idle), std::memory_order_release);
}

CROSSPAD_COLD void SamplerEngine::startPreview(uint32_t gen)
{
    /* The record, if it can be had whole; a torn copy is tried again on the
     * next pass, with the generation still not taken. */
    PreviewReq r;
    const uint32_t s1 = pSeq_.load(std::memory_order_acquire);
    if (s1 & 1u) return;
    std::memcpy(&r, &pReq_, sizeof(r));
    std::atomic_thread_fence(std::memory_order_acquire);
    if (pSeq_.load(std::memory_order_relaxed) != s1) return;
    r.path[sizeof(r.path) - 1] = '\0';
    pDoneGen_.store(gen, std::memory_order_release);

    SampleDesc d;
    if (!pSrc_->describe(r.path, d) || !d.valid()) return;
    const uint32_t total = d.frames;
    uint32_t first = r.first < total ? r.first : 0;
    uint32_t last  = (r.last == 0 || r.last > total) ? total : r.last;
    if (first >= last) { first = 0; last = total; }

    /* There is no head, so the ring starts one frame early: the
     * interpolator's left tap on the first frame is the frame before it. */
    const uint32_t streamStart = first ? first - 1 : 0;
    std::snprintf(pPath_, sizeof(pPath_), "%s", r.path);
    if (!pWindow_.open(*pool_, pPath_, d.channels, streamStart, last)) {
        cStreamRefused_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    pCursor_.bind(nullptr, 0, d.channels, last, &pWindow_);

    const double ratio = static_cast<double>(d.sampleRate) / static_cast<double>(sampleRate_);
    pInc_ = static_cast<uint64_t>(ratio * static_cast<double>(kOne));
    if (pInc_ == 0) pInc_ = 1;
    pPhase_ = static_cast<uint64_t>(first) * kOne;
    pLast_  = last;
    float pl = 0, pr = 0;
    SamplerKit::panGains(r.balance, pl, pr);
    pGainL_ = r.gain * pl;
    pGainR_ = r.gain * pr;
    pEnv_ = 0.0f;
    const float attackFrames = params_.attackMs * 0.001f * static_cast<float>(sampleRate_);
    pEnvStep_ = attackFrames > 1.0f ? (1.0f / attackFrames) : 1.0f;
    pFade_.store(false, std::memory_order_relaxed);
    pGap_ = 0;
    pPrimePasses_ = 0;
    pPlayFrame_.store(first, std::memory_order_relaxed);
    /* Sounds once a block is in, or the whole of a region shorter than one. */
    const uint32_t oneBlock = streamStart + pWindow_.blockFrames();
    pPrimeTo_ = oneBlock < last ? oneBlock : last;
    pCurGen_ = gen;
    pState_.store(static_cast<uint8_t>(PreviewState::Priming), std::memory_order_release);
}

void SamplerEngine::servicePreview()
{
    if (!pool_ || !pSrc_) return;
    /* serviceIo() has more than one caller -- the streaming task, and a
     * teardown waiting for the voices to end -- and the audition's ring is
     * not something two of them may open, fill or close at once. The second
     * one simply skips it; the next pass is a millisecond away. */
    if (pSvc_.exchange(true, std::memory_order_acquire)) return;

    uint8_t st = pState_.load(std::memory_order_acquire);
    const uint32_t req  = pReqGen_.load(std::memory_order_acquire);
    const uint32_t stop = pStopGen_.load(std::memory_order_acquire);

    if (st == static_cast<uint8_t>(PreviewState::Finished)) {
        closePreview();
        st = static_cast<uint8_t>(PreviewState::Idle);
    }
    if (st == static_cast<uint8_t>(PreviewState::Priming) ||
        st == static_cast<uint8_t>(PreviewState::Playing)) {
        /* Stopped, a newer one is waiting, or the loader is retiring the
         * pool this ring is in: this one goes. */
        if (stop == pCurGen_ || req != pCurGen_ || mutating_.load(std::memory_order_acquire)) {
            if (st == static_cast<uint8_t>(PreviewState::Priming)) {
                closePreview();
                st = static_cast<uint8_t>(PreviewState::Idle);
            } else {
                pFade_.store(true, std::memory_order_release);   // the render fades it out
            }
        }
    }
    if (st == static_cast<uint8_t>(PreviewState::Idle)) {
        if (req != pDoneGen_.load(std::memory_order_acquire)) {
            if (req == stop || mutating_.load(std::memory_order_acquire)) {
                /* Stopped before it started, or asked for under a kit that
                 * is being retired: dropped, not kept for afterwards. */
                pDoneGen_.store(req, std::memory_order_release);
            } else {
                startPreview(req);
                st = pState_.load(std::memory_order_acquire);
            }
        }
    }
    if ((st == static_cast<uint8_t>(PreviewState::Priming) ||
         st == static_cast<uint8_t>(PreviewState::Playing)) && pWindow_.isOpen()) {
        pWindow_.service(*pSrc_, pPlayFrame_.load(std::memory_order_acquire));
        if (st == static_cast<uint8_t>(PreviewState::Priming)) {
            if (pWindow_.validUpTo() >= pPrimeTo_) {
                pState_.store(static_cast<uint8_t>(PreviewState::Playing), std::memory_order_release);
            } else if (++pPrimePasses_ > kPreviewPrimeMaxPasses) {
                /* The card never gave the first block: given up, as a voice
                 * refused its ring is. */
                cStreamRefused_.fetch_add(1, std::memory_order_relaxed);
                closePreview();
            }
        }
    }
    pSvc_.store(false, std::memory_order_release);
}

bool SamplerEngine::serviceIo()
{
    servicePreview();
    if (!pool_ || !store_) return true;
    /* One pass in the voices at a time. Two of them -- audio_bg and a
     * teardown's wait -- would each fill the same window, and one could close
     * a ring the other is reading the card into. The second simply leaves the
     * voices to the first and says so; the streaming task's next pass is a
     * millisecond away, and a teardown tries again. */
    if (vSvc_.exchange(true, std::memory_order_acquire)) return false;
    ISampleSource* src = store_->source();
    uint8_t waiting = 0;
    for (auto& v : voices_) {
        const uint8_t st = v.state.load(std::memory_order_acquire);
        if (st == static_cast<uint8_t>(VoiceState::Finished)) {
            /* The render thread has let this one go. Its blocks come back here
             * because this thread is allowed to touch the pool's free list
             * without being on a 1.45 ms clock. */
            v.window.close(*pool_);
            v.cursor.clear();
            v.wantStream = false;
            v.slot = nullptr;
            v.state.store(static_cast<uint8_t>(VoiceState::Free), std::memory_order_release);
            continue;
        }
        if (st == static_cast<uint8_t>(VoiceState::Armed)) {
            /* A touch whose end was never heard. Ended here, freed on the
             * next pass like any Finished voice. */
            if (now_ && now_() - v.armUs > kArmTimeoutMs * 1000u && dropArmed(v)) {
                cArmExpired_.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }
        if (st != static_cast<uint8_t>(VoiceState::Playing)) continue;
        if (!src) continue;
        /* Open the ring the note asked for, here rather than there. No ring
         * now is asked again on the next pass while the head still plays --
         * the platform grows the pool meanwhile (ringsWaiting()). Out of
         * time, it is a voice that plays its resident head and stops -- a
         * short note rather than a glitch -- and it is counted. */
        if (v.wantStream && !v.window.isOpen()) {
            const SampleStore::Slot* sl = v.slot;
            const uint32_t headEnd = sl ? sl->headStart + sl->headFrames : 0;
            if (sl && v.window.open(*pool_, sl->path, sl->desc.channels, headEnd, v.last,
                                    params_.streamBlocks, v.loop ? v.first : StreamWindow::kNoLoop)) {
                v.wantStream = false;
            } else if (sl && v.playFrame.load(std::memory_order_acquire) + kRingRetryFrames < headEnd) {
                ++waiting;
                continue;
            } else {
                v.wantStream = false;
                cStreamRefused_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
        }
        if (!v.window.isOpen()) continue;
        v.window.service(*src, v.playFrame.load(std::memory_order_acquire));
    }
    ringsWaiting_.store(waiting, std::memory_order_release);
    vSvc_.store(false, std::memory_order_release);
    return true;
}

bool SamplerEngine::ioIdle() const
{
    return !vSvc_.load(std::memory_order_acquire) && !pSvc_.load(std::memory_order_acquire);
}

CROSSPAD_COLD bool SamplerEngine::abandonVoices()
{
    /* The voices' pass is ours for the length of this, so no streaming pass
     * fills a window this closes; one that holds it now is waited for by the
     * caller, not here -- it may be below us on the same core. */
    if (vSvc_.exchange(true, std::memory_order_acquire)) return false;
    for (auto& v : voices_) {
        if (v.state.load(std::memory_order_acquire) == static_cast<uint8_t>(VoiceState::Free)) continue;
        /* Playing (the render never ended it) or Finished (ended, ring not
         * yet back): either way its blocks go back here, once -- Free is what
         * stops the next pass handing a Finished voice's ring back again. */
        if (pool_) v.window.close(*pool_);
        v.cursor.clear();
        v.wantStream = false;
        v.slot = nullptr;
        v.fading.store(false, std::memory_order_relaxed);
        v.releasing.store(false, std::memory_order_relaxed);
        v.state.store(static_cast<uint8_t>(VoiceState::Free), std::memory_order_release);
    }
    /* No render ran endVoice() for these, so their pads still read as
     * sounding -- and a teardown waiting on anySounding() would wait for good. */
    for (auto& p : padVoices_) p.store(0, std::memory_order_release);
    vSvc_.store(false, std::memory_order_release);
    return true;
}

CROSSPAD_COLD SamplerEngine::Counters SamplerEngine::counters() const
{
    Counters c;
    c.notesStarted  = cNotes_.load(std::memory_order_relaxed);
    c.silenced      = cSilenced_.load(std::memory_order_relaxed);
    c.pitchedRunFrames  = cPitchedRun_.load(std::memory_order_relaxed);
    c.pitchedSlowFrames = cPitchedSlow_.load(std::memory_order_relaxed);
    c.noteOnMaxUs   = cNoteOnMaxUs_.load(std::memory_order_relaxed);
    c.notesDropped  = cDropped_.load(std::memory_order_relaxed);
    c.steals        = cSteals_.load(std::memory_order_relaxed);
    c.streamRefused = cStreamRefused_.load(std::memory_order_relaxed);
    c.underruns     = cUnderruns_.load(std::memory_order_relaxed);
    c.voicesPeak    = cVoicesPeak_.load(std::memory_order_relaxed);
    c.sheds         = cSheds_.load(std::memory_order_relaxed);
    c.armed         = cArmed_.load(std::memory_order_relaxed);
    c.armFired      = cArmFired_.load(std::memory_order_relaxed);
    c.armCancelled  = cArmCancelled_.load(std::memory_order_relaxed);
    c.armExpired    = cArmExpired_.load(std::memory_order_relaxed);
    c.loopsQueued   = cLoopsQueued_.load(std::memory_order_relaxed);
    c.loopsLaunched = cLoopsLaunched_.load(std::memory_order_relaxed);
    return c;
}

CROSSPAD_COLD void SamplerEngine::resetCounters()
{
    cNotes_ = 0; cDropped_ = 0; cSteals_ = 0; cSheds_ = 0;
    cStreamRefused_ = 0; cUnderruns_ = 0; cVoicesPeak_ = 0; cSilenced_ = 0;
    cPitchedRun_ = 0; cPitchedSlow_ = 0;
    cNoteOnMaxUs_ = 0;
    cArmed_ = 0; cArmFired_ = 0; cArmCancelled_ = 0; cArmExpired_ = 0;
    cLoopsQueued_ = 0; cLoopsLaunched_ = 0;
}

} // namespace crosspad

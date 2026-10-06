// SPDX-License-Identifier: MIT
#include "crosspad/instrument/PitchedInstrument.hpp"
#include "crosspad/dsp/DspMath.hpp"

#include <algorithm>
#include <cmath>

namespace crosspad {

namespace {

constexpr float kInt16Norm = 1.0f / 32768.0f;
constexpr uint64_t kOne = 1ull << 32;              // 32.32 unity
constexpr float kFracNorm = 1.0f / 4294967296.0f;  // low 32 bits of a phase -> [0, 1)
constexpr float kMsToSeconds = 0.001f;
constexpr float kVelocityFullScale = 127.0f;
/// Pad volume and pan are stored as the kit writes them: 0..100 and -100..+100.
constexpr int kPadVolumeMax = 100;
constexpr int kPadPanMax = 100;
/// Mix a note that did not come from the grid at full volume, dead centre.
constexpr int kUngriddedVolume = kPadVolumeMax;
constexpr int kUngriddedPan = 0;
/// Range a zone root may be edited to, in (fractional) MIDI notes.
constexpr float kMinRootMidi = 0.0f;
constexpr float kMaxRootMidi = 127.0f;
/// Equal-tempered pitch: A4 = 440 Hz sits at MIDI note 69.
constexpr float kA4Hz = 440.0f;
constexpr float kA4Note = 69.0f;
constexpr float kSemitonesPerOctave = 12.0f;

float noteToHz(uint8_t midiNote) {
    return kA4Hz * std::pow(2.0f, (static_cast<float>(midiNote) - kA4Note) / kSemitonesPerOctave);
}

int16_t tapAt(const Zone& z, int64_t i) {
    if (i < 0 || i >= static_cast<int64_t>(z.length)) return 0;
    return z.pcm[i];
}

inline float hermite4(float y0, float y1, float y2, float y3, float f) {
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

/// Cubic Hermite (Catmull-Rom) over four int16 taps, bounds-checked. Used
/// only at the edges; the render loop reads the interior taps directly.
float hermite(const Zone& z, int64_t i, float f) {
    return hermite4(tapAt(z, i - 1), tapAt(z, i), tapAt(z, i + 1), tapAt(z, i + 2), f);
}

int clampInt(int v, int lo, int hi) { return (v < lo) ? lo : (v > hi ? hi : v); }

} // namespace

PitchedInstrument::PitchedInstrument() {
    for (uint8_t p = 0; p < kPads; ++p) {
        padVolume_[p].store(kPadVolumeMax, std::memory_order_relaxed);
        padPan_[p].store(0, std::memory_order_relaxed);
    }
}

void PitchedInstrument::beginMutation() {
    mutating_.store(true, std::memory_order_seq_cst);
}

void PitchedInstrument::endMutation() {
    mutating_.store(false, std::memory_order_seq_cst);
}

bool PitchedInstrument::gateEnter(uint32_t maxSpins, void (*yield)()) {
    gated_.store(true, std::memory_order_seq_cst);
    for (uint32_t i = 0; i < maxSpins; ++i) {
        if (!rendering_.load(std::memory_order_seq_cst)) return true;
        if (yield) yield();
    }
    // One last look after the final yield, so maxSpins == 0 still answers for
    // the common case: a renderer that was never in mix() to begin with.
    // The gate stays held either way — dropping it would let the renderer back
    // in while the caller is still swapping. The caller is told whether the
    // renderer was seen to leave, which is what decides whether the old bank
    // can go.
    return !rendering_.load(std::memory_order_seq_cst);
}

void PitchedInstrument::gateExit() {
    gated_.store(false, std::memory_order_seq_cst);
}

SampleBank* PitchedInstrument::installBank(SampleBank* bank, const InstrumentParams& p,
                                           const KitInfo* padMixFrom, uint32_t maxSpins,
                                           void (*yield)(), bool* safeToFree) {
    // The gate is the whole safety argument for freeing the returned bank.
    // While it is held the RT task contributes silence without touching the
    // engine, so by the time setBank() has published the new pointer nobody is
    // inside a render that could still be reading the previous bank's PCM. The
    // mutation flag closes the other door: noteOn/noteOff arrive from the
    // event task, which the gate does not stop, and a note published between
    // allNotesOff() and setBank() would be a voice pointing into a freed bank.
    const bool renderQuiet = gateEnter(maxSpins, yield);
    SampleBank* old = bank_.load(std::memory_order_acquire);

    beginMutation();
    allNotesOff();
    setBank(bank);
    setParams(p);
    if (padMixFrom) {
        for (uint8_t pad = 0; pad < kPads && pad < KIT_PADS; ++pad) {
            setPadMix(pad, padMixFrom->pads[pad].volume, padMixFrom->pads[pad].pan);
        }
    }
    endMutation();
    gateExit();

    if (safeToFree) *safeToFree = renderQuiet;
    return old;
}

bool PitchedInstrument::editZone(uint8_t zoneIdx, float rootMidi, uint32_t loopStart,
                                 uint32_t loopEnd, uint32_t maxSpins, void (*yield)()) {
    // Everything is checked before anything moves: a rejected edit must leave
    // the zone exactly as the renderer already knows it.
    SampleBank* bank = bank_.load(std::memory_order_acquire);
    if (!bank || zoneIdx >= bank->zoneCount()) return false;
    if (std::isnan(rootMidi) || rootMidi < kMinRootMidi || rootMidi > kMaxRootMidi) return false;
    if (!SampleBank::loopValid(bank->zone(zoneIdx).length, loopStart, loopEnd)) return false;

    // The same bracket installBank() uses, for the same reason: a voice reads
    // the loop bounds every block. Nothing is freed here, so gateEnter()'s
    // answer carries no decision — the notes are silenced either way.
    gateEnter(maxSpins, yield);
    beginMutation();
    allNotesOff();
    const bool ok = bank->editZone(zoneIdx, rootMidi, loopStart, loopEnd);
    endMutation();
    gateExit();
    return ok;
}

void PitchedInstrument::setBank(SampleBank* bank) {
    // Voices hold Zone pointers into the old bank; silence them before the
    // swap. reset() keeps Voice::zone as it is on purpose -- see its comment.
    for (uint8_t i = 0; i < kMaxVoices; ++i) voices_[i].reset();
    bank_.store(bank, std::memory_order_release);
}

void PitchedInstrument::setVoiceCap(uint8_t cap) {
    voiceCap_.store(static_cast<uint8_t>(clampInt(cap, 1, kMaxSounding)), std::memory_order_relaxed);
}

void PitchedInstrument::setParams(const InstrumentParams& p) {
    uint8_t mv = p.maxVoices;
    if (mv == 0) mv = 1;
    if (mv > kMaxSounding) mv = kMaxSounding;
    const uint8_t cap = voiceCap_.load(std::memory_order_relaxed);
    if (mv > cap) mv = cap;
    maxVoices_.store(mv, std::memory_order_relaxed);
    attackMs_.store(p.attackMs < 0.0f ? 0.0f : p.attackMs, std::memory_order_relaxed);
    const float rel = p.releaseMs < 0.0f ? 0.0f : p.releaseMs;
    releaseMs_.store(rel, std::memory_order_relaxed);

    // Lowering the cap has to retire what is now over it. Oldest first, the
    // order a steal would pick, but through the normal release: this is a
    // parameter change, not a note fighting for a slot. Retiring is what makes
    // the loop terminate — a retired voice stops counting as sounding.
    for (;;) {
        uint8_t sounding = 0;
        int oldest = -1;
        uint64_t oldestOrder = 0;
        for (uint8_t i = 0; i < kMaxVoices; ++i) {
            Voice& v = voices_[i];
            if (!v.active.load(std::memory_order_relaxed) || v.fading) continue;
            ++sounding;
            if (oldest < 0 || v.order < oldestOrder) {
                oldest = i;
                oldestOrder = v.order;
            }
        }
        if (sounding <= mv || oldest < 0) break;
        retire(voices_[oldest], rel);
    }
}

void PitchedInstrument::setPadMix(uint8_t pad, int volume, int pan) {
    if (pad >= kPads) return;
    padVolume_[pad].store(static_cast<int16_t>(clampInt(volume, 0, kPadVolumeMax)),
                          std::memory_order_relaxed);
    padPan_[pad].store(static_cast<int16_t>(clampInt(pan, -kPadPanMax, kPadPanMax)),
                       std::memory_order_relaxed);
}

void PitchedInstrument::retire(Voice& v, float ms) {
    const float samples = ms * static_cast<float>(sampleRate_) * kMsToSeconds;
    v.fading = true;
    v.releasing = true;
    // Below one sample there is no ramp to run: the step is the whole envelope
    // and the voice is gone on the next frame it renders.
    v.releaseStep = (samples >= 1.0f) ? (v.env / samples) : v.env;
}

int PitchedInstrument::findVoiceSlot() {
    const uint8_t maxV = maxVoices_.load(std::memory_order_relaxed);
    const bool overloaded = overloaded_.load(std::memory_order_relaxed);

    // One pass answers all three questions: how many voices count against the
    // cap, where a free slot is, and which of the counted ones gives way.
    // That is a voice already in its release before a held one -- taking a
    // held note out of a chord is the steal anyone hears, a tail on its way
    // out mostly is not -- and the oldest of those.
    // The rank is the allocation order with the top bit set for a held note:
    // the lowest rank gives way.
    constexpr uint64_t kHeld = 1ull << 63;
    uint8_t sounding = 0;
    int freeSlot = -1;
    int victim = -1;
    uint64_t victimRank = 0;
    for (uint8_t i = 0; i < kMaxVoices; ++i) {
        Voice& v = voices_[i];
        if (!v.active.load(std::memory_order_relaxed)) {
            if (freeSlot < 0) freeSlot = i;
            continue;
        }
        if (v.fading) continue;
        ++sounding;
        const uint64_t rank = v.releasing ? v.order : (v.order | kHeld);
        if (victim < 0 || rank < victimRank) {
            victim = i;
            victimRank = rank;
        }
    }
    // Over budget, a note may not add a voice: it takes one's place.
    const bool shed = overloaded && sounding > 0 && sounding < maxV;
    if (sounding < maxV && !shed && freeSlot >= 0) return freeSlot;

    // At the cap: the chosen voice gives way. It keeps its own slot and fades
    // there, so the new note takes a free one — kFadeSlots exist so that
    // there is one.
    if (victim >= 0) retire(voices_[victim], kStealFadeMs);
    steals_.fetch_add(1, std::memory_order_relaxed);
    if (shed) sheds_.fetch_add(1, std::memory_order_relaxed);
    if (freeSlot >= 0) return freeSlot;

    // Steals arriving faster than kStealFadeMs can retire them: every slot is
    // busy and the only thing left to take is a fade in progress. Cutting the
    // oldest of those is a click on a voice that is already most of the way
    // down, which is still the better answer than dropping the note.
    int oldestFading = -1;
    uint64_t oldestOrder = 0;
    for (uint8_t i = 0; i < kMaxVoices; ++i) {
        Voice& v = voices_[i];
        if (!v.active.load(std::memory_order_relaxed) || !v.fading) continue;
        if (oldestFading < 0 || v.order < oldestOrder) {
            oldestFading = i;
            oldestOrder = v.order;
        }
    }
    if (oldestFading < 0) return -1;
    voices_[oldestFading].reset();
    return oldestFading;
}

void PitchedInstrument::noteOn(uint8_t midiNote, uint8_t velocity, uint8_t pad) {
    if (mutating_.load(std::memory_order_seq_cst)) return;
    SampleBank* bank = bank_.load(std::memory_order_acquire);
    if (!bank) return;
    const Zone* z = bank->nearestZone(static_cast<float>(midiNote));
    if (!z || !z->pcm || z->length == 0) return;

    const float rel = releaseMs_.load(std::memory_order_relaxed);
    const float relSamples = rel * static_cast<float>(sampleRate_) * kMsToSeconds;

    // Retrigger: the sounding voice on this note releases rather than cuts.
    for (uint8_t i = 0; i < kMaxVoices; ++i) {
        if (voices_[i].active.load(std::memory_order_relaxed)
            && !voices_[i].releasing && voices_[i].note == midiNote) {
            voices_[i].releasing = true;
            voices_[i].releaseStep = (relSamples > 0.0f) ? (voices_[i].env / relSamples) : voices_[i].env;
        }
    }

    const int slot = findVoiceSlot();
    if (slot < 0) return;
    Voice& v = voices_[slot];

    const double semis = static_cast<double>(midiNote) - z->rootMidi;
    const double ratio = std::pow(2.0, semis / 12.0);
    v.zone = z;
    v.phase = static_cast<uint64_t>(z->loopStart) * kOne;
    v.inc = static_cast<uint64_t>(ratio * static_cast<double>(kOne) + 0.5);
    if (v.inc == 0) v.inc = 1;
    v.order = nextOrder_++;
    v.note = midiNote;
    v.releasing = false;
    v.fading = false;

    const int vol = (pad < kPads) ? padVolume_[pad].load(std::memory_order_relaxed)
                                  : kUngriddedVolume;
    const int pan = (pad < kPads) ? padPan_[pad].load(std::memory_order_relaxed)
                                  : kUngriddedPan;
    v.gain = (static_cast<float>(velocity) / kVelocityFullScale)
           * (static_cast<float>(vol) / static_cast<float>(kPadVolumeMax));

    // Equal-power pan: the pan range maps onto a quarter turn, so -100 is hard
    // left, +100 hard right and centre is -3 dB on both.
    const float theta = static_cast<float>(pan + kPadPanMax)
                      / static_cast<float>(2 * kPadPanMax)
                      * (static_cast<float>(kPi) * 0.5f);
    v.gainL = std::cos(theta);
    v.gainR = std::sin(theta);

    const float atk = attackMs_.load(std::memory_order_relaxed);
    const float atkSamples = atk * static_cast<float>(sampleRate_) * kMsToSeconds;
    if (atkSamples >= 1.0f) {
        v.env = 0.0f;
        v.attackStep = 1.0f / atkSamples;
    } else {
        v.env = 1.0f;
        v.attackStep = 0.0f;
    }
    v.releaseStep = (relSamples > 0.0f) ? (1.0f / relSamples) : 1.0f;

    // The audio thread only looks at a voice once it is active; the release
    // store makes everything written above visible before that flag is, or the
    // first block would render with the gain and envelope the slot held before.
    v.active.store(true, std::memory_order_release);

    // The mutation flag is checked on both sides of the publish. The entry
    // check rejects a note that arrives while the loader is working; this one
    // closes the other window -- a noteOn that passed the entry check, then
    // got descheduled, and would otherwise publish a voice pinning a zone of
    // the bank the loader is about to free (its allNotesOff() already ran).
    // Whichever of the two orderings the sequentially consistent flag lands
    // in, the note is dropped rather than left pointing at freed PCM.
    if (mutating_.load(std::memory_order_seq_cst)) {
        v.active.store(false, std::memory_order_relaxed);
        return;
    }

    lastNoteHz_.store(noteToHz(midiNote), std::memory_order_relaxed);
}

void PitchedInstrument::noteOff(uint8_t midiNote) {
    if (mutating_.load(std::memory_order_seq_cst)) return;
    const float rel = releaseMs_.load(std::memory_order_relaxed);
    const float relSamples = rel * static_cast<float>(sampleRate_) * kMsToSeconds;
    for (uint8_t i = 0; i < kMaxVoices; ++i) {
        Voice& v = voices_[i];
        if (v.active.load(std::memory_order_relaxed) && !v.releasing && v.note == midiNote) {
            v.releasing = true;
            v.releaseStep = (relSamples > 0.0f) ? (v.env / relSamples) : v.env;
        }
    }
}

void PitchedInstrument::allNotesOff() {
    for (uint8_t i = 0; i < kMaxVoices; ++i) voices_[i].reset();
}

void PitchedInstrument::onPrepare(uint32_t maxFrames, uint32_t sampleRate) {
    maxFrames_ = maxFrames;
    if (sampleRate > 0) sampleRate_ = sampleRate;
    scratch_.assign(maxFrames, 0.0f);
    panTmp_.assign(maxFrames, 0.0f);
    monoSum_.assign(maxFrames, 0.0f);
    scopeTap_.setSampleRate(sampleRate_);
}

void PitchedInstrument::renderVoice(Voice& v, uint32_t frames) {
    // One load of the zone pointer for the whole block: mix() checked it is
    // non-null, and reset() never clears it, but a re-read would let the
    // compiler observe a slot another task has meanwhile written.
    const Zone* const zp = v.zone;
    const Zone& z = *zp;
    const bool looping = (z.loopEnd > z.loopStart);
    const uint64_t loopStartQ = static_cast<uint64_t>(z.loopStart) * kOne;
    const uint64_t loopSpanQ = looping
        ? (static_cast<uint64_t>(z.loopEnd - z.loopStart) * kOne) : 0;
    float* dst = scratch_.data();

    // Taps i-1..i+2 are in range for 1 <= i <= length-3; the loop takes the
    // unchecked read there (four loads, no branches) and the bounds-checked
    // hermite() only on the first and last samples. Measured on the S3: the
    // checked version cost 72 us per voice per 64-frame block.
    const int64_t lastFast = static_cast<int64_t>(z.length) - 3;
    const int16_t* pcm = z.pcm;
    const float scale = kInt16Norm * v.gain;

    for (uint32_t n = 0; n < frames; ++n) {
        if (!v.active.load(std::memory_order_relaxed)) { dst[n] = 0.0f; continue; }

        const int64_t i = static_cast<int64_t>(v.phase >> 32);
        const float frac = static_cast<float>(static_cast<uint32_t>(v.phase))
                         * kFracNorm;
        float y;
        if (i >= 1 && i <= lastFast) {
            const int16_t* t = pcm + i - 1;
            y = hermite4(t[0], t[1], t[2], t[3], frac);
        } else {
            y = hermite(z, i, frac);
        }
        dst[n] = y * scale * v.env;

        // envelope
        if (v.releasing) {
            v.env -= v.releaseStep;
            if (v.env <= 0.0f) {
                v.env = 0.0f;
                v.active.store(false, std::memory_order_relaxed);
            }
        } else if (v.attackStep > 0.0f && v.env < 1.0f) {
            v.env += v.attackStep;
            if (v.env > 1.0f) v.env = 1.0f;
        }

        v.phase += v.inc;
        if (looping) {
            // Modulo, not repeated subtraction: SampleBank rejects loops under
            // four samples, but a legal short loop played far above its root
            // still wraps many times per frame, and the subtract loop cost is
            // linear in that count.
            if (v.phase >= loopStartQ + loopSpanQ) {
                v.phase = loopStartQ + (v.phase - loopStartQ) % loopSpanQ;
            }
        } else if ((v.phase >> 32) + 1 >= static_cast<uint64_t>(z.length)) {
            v.active.store(false, std::memory_order_relaxed);
        }
    }
}

void PitchedInstrument::mix(float* out, uint32_t frames) {
    // Publish "inside the engine" before reading the gate, so a mutator
    // cannot conclude the renderer is out while it is on its way in. Both
    // stores happen even on the early returns below — the flag has to be
    // honest about every path through mix(), not just the one that renders.
    rendering_.store(true, std::memory_order_seq_cst);
    if (gated_.load(std::memory_order_seq_cst)) {
        rendering_.store(false, std::memory_order_seq_cst);
        // A gated block still happened, and it was silent. Say so, or the view
        // would hold the last waveform for the whole kit swap.
        publishScope(frames, true);
        return;
    }
    renderBlock(out, frames);   // publishes its own mono sum
    rendering_.store(false, std::memory_order_seq_cst);
}

void PitchedInstrument::publishScope(uint32_t frames, bool silent) {
    if (!scopeTap_.enabled() || monoSum_.empty() || frames == 0) return;
    if (frames > maxFrames_) frames = maxFrames_;
    if (silent) std::fill(monoSum_.begin(), monoSum_.begin() + frames, 0.0f);
    scopeTap_.write(monoSum_.data(), frames);
}

void PitchedInstrument::renderBlock(float* out, uint32_t frames) {
    if (!out || frames == 0) return;
    if (maxFrames_ == 0 || scratch_.empty()) return;
    if (frames > maxFrames_) frames = maxFrames_;

    // The scope sum is built only while a live view consumes it. `out` is the
    // mixer's bus and already carries whatever ran before this node.
    const bool captureScope = scopeTap_.enabled();
    float* mono = captureScope ? monoSum_.data() : nullptr;
    if (captureScope) std::fill(mono, mono + frames, 0.0f);

    if (bank_.load(std::memory_order_acquire)) {
        for (uint8_t i = 0; i < kMaxVoices; ++i) {
            Voice& v = voices_[i];
            // Pairs with the release store in noteOn(): a voice seen active
            // here has its zone, phase and gains fully written. A slot that
            // has never been used still has a null zone, so it is skipped.
            if (!v.active.load(std::memory_order_acquire) || !v.zone) continue;
            renderVoice(v, frames);
            if (captureScope) {
                for (uint32_t n = 0; n < frames; ++n) mono[n] += scratch_[n];
            }
            detail::accumulatePanned(scratch_.data(), out, frames, v.gainL, v.gainR,
                                     panTmp_.data());
        }
    }

    if (captureScope) publishScope(frames, false);
}

uint8_t PitchedInstrument::activeVoices() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < kMaxVoices; ++i) {
        if (voices_[i].active.load(std::memory_order_relaxed)) ++n;
    }
    return n;
}

uint8_t PitchedInstrument::soundingVoices() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < kMaxVoices; ++i) {
        const Voice& v = voices_[i];
        if (v.active.load(std::memory_order_relaxed) && !v.fading) ++n;
    }
    return n;
}

} // namespace crosspad

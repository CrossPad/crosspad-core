// SPDX-License-Identifier: MIT
#include "crosspad/sequencer/PatternSequencer.hpp"

namespace crosspad {

// ── Input ──────────────────────────────────────────────────────────────────

void PatternSequencer::noteIn(uint8_t pad, uint8_t velocity)
{
    if (!clock_) return;
    const uint32_t head = ringHead_.load(std::memory_order_relaxed);
    const uint32_t next = (head + 1) % kInputRing;
    /* Full means poll() has stalled; drop rather than block the pad thread.
     * A dropped hit is a missing note in the take, which is recoverable --
     * a pad that waits on a mutex is a missed beat, which is not. */
    if (next == ringTail_.load(std::memory_order_acquire)) return;

    ring_[head] = { clock_->positionSamples(), pad, velocity };
    ringHead_.store(next, std::memory_order_release);
}

// ── Grid ───────────────────────────────────────────────────────────────────

uint32_t PatternSequencer::quantise(uint32_t pos, uint32_t loop) const
{
    if (quant_ == Quantization::Off || !clock_) return pos;

    uint32_t div = 4;
    switch (quant_) {
        case Quantization::Quarter:      div = 1; break;
        case Quantization::Eighth:       div = 2; break;
        case Quantization::Sixteenth:    div = 4; break;
        case Quantization::ThirtySecond: div = 8; break;
        default: return pos;
    }
    const uint32_t grid = clock_->samplesPerBeat() / div;
    if (grid < 2) return pos;

    const uint32_t snapped = ((pos + grid / 2) / grid) * grid;
    /* A hit a few samples before the loop end rounds up to the loop end, which
     * is the same instant as the start -- put it where it is heard. */
    return (loop && snapped >= loop) ? 0 : snapped;
}

void PatternSequencer::addEvent(uint8_t seq, uint32_t pos, uint8_t pad, uint8_t vel)
{
    if (seq >= kNumSequences || counts_[seq] >= kMaxEvents) return;
    events_[seq][counts_[seq]++] = { pos, pad, vel, 1 };
    markDirty();
}

void PatternSequencer::drainInput(uint32_t loop)
{
    uint32_t tail = ringTail_.load(std::memory_order_relaxed);
    const uint32_t head = ringHead_.load(std::memory_order_acquire);
    const bool recording = (mode_ == SequencerMode::Record || mode_ == SequencerMode::Overdub)
                           && state_ == SequencerState::Running;

    while (tail != head) {
        const Hit& h = ring_[tail];
        /* Into the scene that is sounding: you record into what you hear. */
        if (recording) addEvent(activeSeq_, quantise(h.pos, loop), h.pad, h.vel);
        tail = (tail + 1) % kInputRing;
    }
    ringTail_.store(tail, std::memory_order_release);
}

// ── Playback ───────────────────────────────────────────────────────────────

void PatternSequencer::fireWindow(uint32_t from, uint32_t to)
{
    if (!sink_ || from >= to) return;
    for (uint16_t i = 0; i < counts_[activeSeq_]; ++i) {
        const Event& e = events_[activeSeq_][i];
        if (e.fresh) continue;
        if (e.pos >= from && e.pos < to) sink_(sinkCtx_, e.pad, e.vel);
    }
}

void PatternSequencer::poll()
{
    if (!clock_) return;
    const uint32_t loop = clock_->loopSamples();
    const uint32_t now  = clock_->positionSamples();

    drainInput(loop);

    if (state_ != SequencerState::Running || !clock_->running()) {
        lastPos_ = now;
        return;
    }

    /* Half-open [from, to): every position is covered exactly once across
     * consecutive polls, so nothing fires twice and nothing is skipped. */
    if (now >= lastPos_) {
        fireWindow(lastPos_, now);
    } else {
        fireWindow(lastPos_, loop);
        /* The loop has come round: what was recorded during the pass just
         * ended is now part of the pattern and plays from here on. */
        for (uint8_t s = 0; s < kNumSequences; ++s)
            for (uint16_t i = 0; i < counts_[s]; ++i) events_[s][i].fresh = 0;

        /* The only moment a queued scene may take over. Doing it here rather
         * than when the button was pressed is what keeps a swap on the bar
         * line instead of cutting the pattern in half. */
        if (pendingSeq_ != activeSeq_) applyScene(pendingSeq_);

        fireWindow(0, now);

        if (!loop_) stop();
    }
    lastPos_ = now;
}

// ── Scenes ─────────────────────────────────────────────────────────────────

void PatternSequencer::applyScene(uint8_t seq)
{
    if (seq >= kNumSequences) return;
    activeSeq_  = seq;
    pendingSeq_ = seq;
    /* The scene's length is the loop, and the click counts that same loop --
     * handing it to the metronome is what keeps bar one and the downbeat the
     * same instant. */
    if (clock_) clock_->setBarCount(bars_[seq]);
    lastPos_ = 0;
    markDirty();   // the file records which scene was left playing
}

void PatternSequencer::select(uint8_t seq)
{
    if (seq >= kNumSequences) return;
    if (state_ == SequencerState::Running && clock_ && clock_->running()) {
        pendingSeq_ = seq;          // takes effect at the loop end
        return;
    }
    applyScene(seq);
    if (clock_) clock_->resetPosition();
}

void PatternSequencer::setSceneBars(uint8_t seq, uint8_t bars)
{
    if (seq >= kNumSequences || !bars) return;
    if (bars > kMaxBars) bars = kMaxBars;
    bars_[seq] = bars;
    markDirty();

    if (!clock_) return;
    const uint32_t loop = (uint32_t)clock_->samplesPerBeat() * clock_->beatsPerBar() * bars;
    /* Events past the new loop end would never be reached again; drop them so
     * the count the UI shows is the count that plays. */
    uint16_t kept = 0;
    for (uint16_t i = 0; i < counts_[seq]; ++i) {
        if (events_[seq][i].pos < loop) events_[seq][kept++] = events_[seq][i];
    }
    counts_[seq] = kept;

    if (seq == activeSeq_) {
        clock_->setBarCount(bars);
        if (lastPos_ >= loop) lastPos_ = 0;
    }
}

// ── Transport ──────────────────────────────────────────────────────────────

void PatternSequencer::setTransport(SequencerMode m, SequencerState st)
{
    mode_  = m;
    state_ = st;
    if (!controller_) return;
    controller_->setState(st);   // clears mode on Idle, so mode goes second
    controller_->setMode(m);
}

void PatternSequencer::playback()
{
    if (!clock_) return;
    if (!clock_->running()) clock_->start();
    lastPos_ = clock_->positionSamples();
    setTransport(SequencerMode::Playback, SequencerState::Running);
}

void PatternSequencer::record()
{
    if (!clock_) return;
    /* Record replaces; overdub adds. Clearing here rather than at the wrap is
     * what makes the button honest -- you hear the old take stop immediately. */
    counts_[activeSeq_] = 0;
    markDirty();
    if (!clock_->running()) clock_->start();
    lastPos_ = clock_->positionSamples();
    setTransport(SequencerMode::Record, SequencerState::Running);
}

void PatternSequencer::overdub()
{
    if (!clock_) return;
    if (!clock_->running()) clock_->start();
    lastPos_ = clock_->positionSamples();
    setTransport(SequencerMode::Overdub, SequencerState::Running);
}

void PatternSequencer::stop()
{
    if (clock_) clock_->stop();
    setTransport(SequencerMode::None, SequencerState::Idle);
}

void PatternSequencer::reset()
{
    stop();
    clearAll(0, 127);
    if (clock_) clock_->resetPosition();
    lastPos_ = 0;
}

void PatternSequencer::resetScenes()
{
    for (uint8_t s = 0; s < kNumSequences; ++s) {
        counts_[s]       = 0;
        bars_[s]         = kDefaultBars;
        sceneEnabled_[s] = true;
    }
    applyScene(0);
    if (clock_) clock_->resetPosition();
    lastPos_ = 0;
    clearDirty();
}

// ── Erase ──────────────────────────────────────────────────────────────────

void PatternSequencer::clearAll(uint8_t startPad, uint8_t endPad)
{
    for (uint8_t s = 0; s < kNumSequences; ++s) clearSeq(s, endPad);
    (void)startPad;
}

void PatternSequencer::clearSeq(uint8_t seqIdx, uint8_t endPad)
{
    if (seqIdx >= kNumSequences) return;
    markDirty();
    if (endPad >= 127) { counts_[seqIdx] = 0; return; }

    uint16_t kept = 0;
    for (uint16_t i = 0; i < counts_[seqIdx]; ++i) {
        if (events_[seqIdx][i].pad > endPad) events_[seqIdx][kept++] = events_[seqIdx][i];
    }
    counts_[seqIdx] = kept;
}

// ── Length and readback ────────────────────────────────────────────────────

void PatternSequencer::setLength(uint8_t beats)
{
    if (!clock_) return;
    const uint8_t bpb = clock_->beatsPerBar() ? clock_->beatsPerBar() : 4;
    setSceneBars(activeSeq_, beats / bpb);
}

uint16_t PatternSequencer::eventCount() const
{
    uint16_t n = 0;
    for (uint8_t s = 0; s < kNumSequences; ++s) n = (uint16_t)(n + counts_[s]);
    return n;
}

float PatternSequencer::progress() const
{
    if (!clock_) return 0.0f;
    const uint32_t loop = clock_->loopSamples();
    return loop ? (float)clock_->positionSamples() / (float)loop : 0.0f;
}

// ── Storage ────────────────────────────────────────────────────────────────

bool PatternSequencer::addEventTicks(uint8_t seq, uint32_t ticks, uint8_t pad, uint8_t vel)
{
    if (seq >= kNumSequences || !clock_ || counts_[seq] >= kMaxEvents) return false;
    const uint32_t spb = clock_->samplesPerBeat();
    const uint32_t pos = (uint32_t)(((uint64_t)ticks * spb) / kPpq);
    /* A file written for a longer scene must not smuggle events past this
     * scene's end, where they would never play and would confuse the count. */
    const uint32_t loop = spb * clock_->beatsPerBar() * bars_[seq];
    if (loop && pos >= loop) return false;

    events_[seq][counts_[seq]++] = { pos, pad, vel, 0 };
    return true;
}

bool PatternSequencer::eventAtTicks(uint8_t seq, uint16_t i,
                                    uint32_t& ticks, uint8_t& pad, uint8_t& vel) const
{
    if (seq >= kNumSequences || i >= counts_[seq] || !clock_) return false;
    const uint32_t spb = clock_->samplesPerBeat();
    if (!spb) return false;
    const uint32_t pos = events_[seq][i].pos;
    ticks = (uint32_t)(((uint64_t)pos * kPpq + spb / 2) / spb);
    pad   = events_[seq][i].pad;
    vel   = events_[seq][i].vel;
    return true;
}

} // namespace crosspad

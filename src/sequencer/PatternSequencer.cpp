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
}

void PatternSequencer::drainInput(uint32_t loop)
{
    uint32_t tail = ringTail_.load(std::memory_order_relaxed);
    const uint32_t head = ringHead_.load(std::memory_order_acquire);
    const bool recording = (mode_ == SequencerMode::Record || mode_ == SequencerMode::Overdub)
                           && state_ == SequencerState::Running;

    while (tail != head) {
        const Hit& h = ring_[tail];
        if (recording) addEvent(recSeq_, quantise(h.pos, loop), h.pad, h.vel);
        tail = (tail + 1) % kInputRing;
    }
    ringTail_.store(tail, std::memory_order_release);
}

// ── Playback ───────────────────────────────────────────────────────────────

void PatternSequencer::fireWindow(uint32_t from, uint32_t to)
{
    if (!sink_ || from >= to) return;
    for (uint8_t s = 0; s < kNumSequences; ++s) {
        if (!seqEnabled_[s]) continue;
        for (uint16_t i = 0; i < counts_[s]; ++i) {
            const Event& e = events_[s][i];
            if (e.fresh) continue;
            if (e.pos >= from && e.pos < to) sink_(sinkCtx_, e.pad, e.vel);
        }
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
        fireWindow(0, now);

        if (!loop_) stop();
    }
    lastPos_ = now;
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
    if (recSeq_ < kNumSequences) counts_[recSeq_] = 0;
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

// ── Erase ──────────────────────────────────────────────────────────────────

void PatternSequencer::clearAll(uint8_t startPad, uint8_t endPad)
{
    for (uint8_t s = 0; s < kNumSequences; ++s) clearSeq(s, endPad);
    (void)startPad;
}

void PatternSequencer::clearSeq(uint8_t seqIdx, uint8_t endPad)
{
    if (seqIdx >= kNumSequences) return;
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
    setBars(beats / bpb);
}

void PatternSequencer::setBars(uint8_t bars)
{
    if (!clock_ || !bars) return;
    clock_->setBarCount(bars);
    /* Events past the new loop end would never be reached again; drop them so
     * the count the UI shows is the count that plays. */
    const uint32_t loop = clock_->loopSamples();
    for (uint8_t s = 0; s < kNumSequences; ++s) {
        uint16_t kept = 0;
        for (uint16_t i = 0; i < counts_[s]; ++i) {
            if (events_[s][i].pos < loop) events_[s][kept++] = events_[s][i];
        }
        counts_[s] = kept;
    }
    if (lastPos_ >= loop) lastPos_ = 0;
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

} // namespace crosspad

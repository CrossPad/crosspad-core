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

void PatternSequencer::erasePad(uint8_t seq, uint8_t pad)
{
    if (seq >= kNumSequences) return;
    uint16_t kept = 0;
    for (uint16_t i = 0; i < counts_[seq]; ++i) {
        if (events_[seq][i].pad != pad) events_[seq][kept++] = events_[seq][i];
    }
    if (kept == counts_[seq]) return;
    counts_[seq] = kept;
    markDirty();
}

void PatternSequencer::drainInput(uint32_t loop)
{
    uint32_t tail = ringTail_.load(std::memory_order_relaxed);
    const uint32_t head = ringHead_.load(std::memory_order_acquire);
    const bool recording = (mode_ == SequencerMode::Record || mode_ == SequencerMode::Overdub)
                           && state_ == SequencerState::Running;
    const bool erasing = erasing_.load(std::memory_order_relaxed);

    while (tail != head) {
        const Hit& h = ring_[tail];
        /* Erase held: the hit names a pad to take out, it is not a note. */
        if (erasing)        erasePad(activeSeq_, h.pad);
        /* Into the scene that is sounding: you record into what you hear. */
        else if (recording) addEvent(activeSeq_, quantise(h.pos, loop), h.pad, h.vel);
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
        if (e.pos >= from && e.pos < to) {
            sink_(sinkCtx_, e.pad, e.vel, true);
            if (e.pad < 16) pendingRelease_ |= (uint16_t)(1u << e.pad);
        }
    }
}

void PatternSequencer::poll()
{
    if (!clock_) return;
    std::lock_guard<std::recursive_mutex> lock(editMutex_);

    /* Release what the last pass fired, before anything else. A pad left
     * pressed is not a silent bug: the LED stays lit and every "is this pad
     * pressed" reader answers yes for good -- which is what stopped the
     * sampler's waveform settling on one pad. */
    if (pendingRelease_ && sink_) {
        for (uint8_t pad = 0; pad < 16; ++pad) {
            if (pendingRelease_ & (1u << pad)) sink_(sinkCtx_, pad, 0, false);
        }
        pendingRelease_ = 0;
    }

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
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    if (seq >= sceneCount_) return;
    if (state_ == SequencerState::Running && clock_ && clock_->running()) {
        pendingSeq_ = seq;          // takes effect at the loop end
        return;
    }
    applyScene(seq);
    if (clock_) clock_->resetPosition();
}

void PatternSequencer::setSceneBars(uint8_t seq, uint8_t bars)
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    applySceneBars(seq, bars);
}

void PatternSequencer::applySceneBars(uint8_t seq, uint8_t bars)
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

bool PatternSequencer::doubleScene(uint8_t seq)
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    if (seq >= sceneCount_ || !clock_) return false;
    if (isRecording() && seq == activeSeq_) return false;
    const uint8_t bars = bars_[seq];
    if (!bars || bars * 2 > kMaxBars) return false;
    const uint16_t n = counts_[seq];
    if (n * 2 > kMaxEvents) return false;

    const uint32_t loop = (uint32_t)clock_->samplesPerBeat() * clock_->beatsPerBar() * bars;
    for (uint16_t i = 0; i < n; ++i) {
        const Event& e = events_[seq][i];
        events_[seq][n + i] = { e.pos + loop, e.pad, e.vel, 0 };
    }
    counts_[seq] = (uint16_t)(n * 2);
    applySceneBars(seq, (uint8_t)(bars * 2));
    return true;
}

bool PatternSequencer::halveScene(uint8_t seq)
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    if (seq >= sceneCount_) return false;
    if (isRecording() && seq == activeSeq_) return false;
    if (bars_[seq] < 2) return false;
    applySceneBars(seq, (uint8_t)(bars_[seq] / 2));
    return true;
}

// ── Tempo ──────────────────────────────────────────────────────────────────

void PatternSequencer::setClickBpm(float bpm)
{
    if (!clock_) return;
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    const float    was    = clock_->bpm();
    const uint32_t before = clock_->samplesPerBeat();
    clock_->setBpm(bpm);
    const uint32_t after = clock_->samplesPerBeat();
    /* The file records the tempo its ticks were written at, so a tempo the
     * player changed has to reach the card even when the rescale below finds
     * nothing to move. */
    if (clock_->bpm() != was) markDirty();
    if (!before || !after || before == after) return;

    /* Events are held in samples and a beat is now a different number of them,
     * so a take recorded at 120 and played at 90 would keep its old offsets
     * inside a longer bar: the pattern drifts off the click it was played to,
     * and everything past the old loop end falls outside the new one and never
     * fires again. Scaling them is what makes a kit's tempo a property of the
     * kit rather than something only an empty scene can be given. The file
     * holds ticks for the same reason. */
    for (uint8_t s = 0; s < kNumSequences; ++s) {
        for (uint16_t i = 0; i < counts_[s]; ++i) {
            events_[s][i].pos = (uint32_t)(((uint64_t)events_[s][i].pos * after) / before);
        }
    }
    /* The clock cannot be moved from here, so take its position as read --
     * firing everything between the old playhead and the rescaled one would
     * empty the pattern into a single poll. */
    lastPos_ = clock_->positionSamples();
}

/// Append an empty scene and go to it.
bool PatternSequencer::addScene()
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    if (sceneCount_ >= kNumSequences) return false;
    const uint8_t idx = sceneCount_++;
    counts_[idx] = 0;
    bars_[idx]   = kDefaultBars;
    sceneEnabled_[idx] = true;
    markDirty();
    select(idx);
    return true;
}

/// Drop a scene and close the gap behind it.
///
/// The last one never goes: a kit has to have somewhere to record, and an empty
/// scene is what "no scenes" looks like anyway. Removing the scene that is
/// sounding takes effect at once rather than at the loop end -- there is no
/// loop left to wait for.
bool PatternSequencer::removeScene(uint8_t seq)
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    if (seq >= sceneCount_ || sceneCount_ <= 1) return false;

    for (uint8_t s = seq; s + 1 < sceneCount_; ++s) {
        counts_[s] = counts_[s + 1];
        bars_[s]   = bars_[s + 1];
        sceneEnabled_[s] = sceneEnabled_[s + 1];
        for (uint16_t i = 0; i < counts_[s]; ++i) events_[s][i] = events_[s + 1][i];
    }
    const uint8_t last = --sceneCount_;
    counts_[last] = 0;
    bars_[last]   = kDefaultBars;
    sceneEnabled_[last] = true;
    markDirty();

    uint8_t sel = activeSeq_;
    if (sel >= sceneCount_) sel = (uint8_t)(sceneCount_ - 1);
    applyScene(sel);
    if (clock_) clock_->resetPosition();
    return true;
}

void PatternSequencer::setSceneCount(uint8_t n)
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    if (n < 1) n = 1;
    if (n > kNumSequences) n = kNumSequences;
    sceneCount_ = n;
    if (activeSeq_ >= sceneCount_) applyScene(0);
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
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
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
    /* Whatever is still held goes down with the transport, or it stays held
     * until the next pass that never comes. */
    if (pendingRelease_ && sink_) {
        for (uint8_t pad = 0; pad < 16; ++pad) {
            if (pendingRelease_ & (1u << pad)) sink_(sinkCtx_, pad, 0, false);
        }
        pendingRelease_ = 0;
    }
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
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    for (uint8_t s = 0; s < kNumSequences; ++s) {
        counts_[s]       = 0;
        bars_[s]         = kDefaultBars;
        sceneEnabled_[s] = true;
    }
    /* One scene, the way a kit nobody has recorded into starts. The rest are
     * made when somebody asks for them. */
    sceneCount_ = 1;
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
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
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

bool PatternSequencer::sceneEvent(uint8_t seq, uint16_t i, float& at, uint8_t& pad, uint8_t& vel) const
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    if (seq >= kNumSequences || i >= counts_[seq] || !clock_) return false;
    const uint32_t loop = (uint32_t)clock_->samplesPerBeat() * clock_->beatsPerBar() * bars_[seq];
    if (!loop) return false;
    const Event& e = events_[seq][i];
    at  = (float)e.pos / (float)loop;
    pad = e.pad;
    vel = e.vel;
    return true;
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
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
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
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
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

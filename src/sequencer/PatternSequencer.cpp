// SPDX-License-Identifier: MIT
#include "crosspad/sequencer/PatternSequencer.hpp"

namespace crosspad {

// ── Input ──────────────────────────────────────────────────────────────────

void PatternSequencer::noteIn(uint8_t pad, uint8_t velocity)
{
    pushHit(pad, velocity, true);
}

void PatternSequencer::noteRelease(uint8_t pad)
{
    pushHit(pad, 0, false);
}

void PatternSequencer::pushHit(uint8_t pad, uint8_t vel, bool on)
{
    if (!clock_) return;
    const uint32_t head = ringHead_.load(std::memory_order_relaxed);
    const uint32_t next = (head + 1) % kInputRing;
    /* Full means poll() has stalled; drop rather than block the pad thread.
     * A dropped hit is a missing note in the take, which is recoverable --
     * a pad that waits on a mutex is a missed beat, which is not. */
    if (next == ringTail_.load(std::memory_order_acquire)) return;

    const uint32_t early = clock_->countInSamplesLeft();
    ring_[head] = { early ? 0u : clock_->positionSamples(), early, clock_->elapsedSamples(),
                    pad, vel, (uint8_t)(on ? 1 : 0) };
    ringHead_.store(next, std::memory_order_release);
}

// ── Grid ───────────────────────────────────────────────────────────────────

namespace {
/// Quantisation off still forgives a count-in hit this much early: a 32nd.
constexpr uint32_t kUnquantisedGraceDiv = 8;
}

uint8_t PatternSequencer::gridPerBeat() const
{
    switch (quant_) {
        case Quantization::Quarter:          return 1;
        case Quantization::Eighth:           return 2;
        case Quantization::EighthTriplet:    return 3;
        case Quantization::Sixteenth:        return 4;
        case Quantization::SixteenthTriplet: return 6;
        case Quantization::ThirtySecond:     return 8;
        default:                             return 0;   // off
    }
}

uint32_t PatternSequencer::gridSamples() const
{
    const uint8_t per = gridPerBeat();
    if (!per || !clock_) return 0;
    return clock_->samplesPerBeat() / per;
}

uint32_t PatternSequencer::downbeatGrace() const
{
    const uint32_t grid = gridSamples();
    if (grid) return grid / 2;
    return clock_ ? clock_->samplesPerBeat() / kUnquantisedGraceDiv : 0;
}

uint32_t PatternSequencer::quantise(uint32_t pos, uint32_t loop) const
{
    const uint8_t  per = gridPerBeat();
    const uint32_t spb = clock_ ? clock_->samplesPerBeat() : 0;
    if (!per || spb < 2) return pos;

    /* The nearest line k, then the line itself as k*spb/per -- not as k steps
     * of spb/per. Three to the beat divides no sample count evenly, and a step
     * rounded down is short on every one of them: four bars are forty-eight
     * steps, so the last line of a scene would sit a hundred samples before the
     * beat it belongs to. Straight grids gain the same exactness, since a beat
     * is rarely divisible by four either (22050 / 4 is not). */
    const uint32_t k = (uint32_t)(((uint64_t)pos * per * 2 + spb) / ((uint64_t)spb * 2));
    const uint32_t snapped = (uint32_t)(((uint64_t)k * spb) / per);
    /* A hit a few samples before the loop end rounds up to the loop end, which
     * is the same instant as the start -- put it where it is heard. */
    return (loop && snapped >= loop) ? 0 : snapped;
}

int PatternSequencer::addEvent(uint8_t seq, uint32_t pos, uint8_t pad, uint8_t vel)
{
    if (seq >= kNumSequences || counts_[seq] >= kMaxEvents) return -1;
    const uint16_t i = counts_[seq]++;
    events_[seq][i] = { pos, 0, pad, vel, 1, 0 };
    markDirty();
    return i;
}

void PatternSequencer::closeNote(uint8_t pad, uint32_t elapsed, uint32_t loop)
{
    for (uint8_t s = 0; s < kNumSequences; ++s) {
        for (uint16_t i = 0; i < counts_[s]; ++i) {
            Event& e = events_[s][i];
            if (!e.open || e.pad != pad) continue;
            /* Measured on the unwrapped count, so a hold over the loop end --
             * or over several -- is not mistaken for a short one. A note
             * cannot outlast the loop it plays in. */
            uint32_t len = elapsed - openAt_[pad];
            if (loop && len > loop) len = loop;
            e.len  = len;
            e.open = 0;
            markDirty();
        }
    }
}

void PatternSequencer::closeAllNotes(uint32_t elapsed, uint32_t loop)
{
    for (uint8_t pad = 0; pad < 16; ++pad) closeNote(pad, elapsed, loop);
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
    const bool taking = (mode_ == SequencerMode::Record || mode_ == SequencerMode::Overdub)
                        && (state_ == SequencerState::Running || state_ == SequencerState::PreBeat);
    const bool erasing = erasing_.load(std::memory_order_relaxed);
    const uint32_t grace = downbeatGrace();

    while (tail != head) {
        const Hit& h = ring_[tail];
        int idx = -1;
        if (!h.on) {
            /* A release only ends a note a take opened; whatever the
             * transport did since, how long the pad was down is the length. */
            if (h.pad < 16) closeNote(h.pad, h.elapsed, loop);
        } else if (erasing) {
            /* Erase held: the hit names a pad to take out, it is not a note. */
            erasePad(activeSeq_, h.pad);
        } else if (taking && h.early) {
            /* Made during the count-in: a hair early for beat one is beat
             * one -- the player aimed at it -- anything earlier was not
             * part of the take. */
            if (h.early <= grace) idx = addEvent(activeSeq_, 0, h.pad, h.vel);
        } else if (taking && state_ == SequencerState::Running) {
            /* Into the scene that is sounding: you record into what you hear --
             * including the swing, so the heard position comes back to the
             * straight grid before it is snapped to it. */
            idx = addEvent(activeSeq_, quantise(unswung(h.pos), loop), h.pad, h.vel);
        }
        /* The length runs from where the pad went down, not from the grid line
         * the note was snapped to: it is how long it was held that is kept. A
         * second press with the first never released ends the first there. */
        if (idx >= 0 && h.pad < 16) {
            closeNote(h.pad, h.elapsed, loop);
            openAt_[h.pad] = h.elapsed;
            events_[activeSeq_][idx].open = 1;
        }
        tail = (tail + 1) % kInputRing;
    }
    ringTail_.store(tail, std::memory_order_release);
}

// ── Playback ───────────────────────────────────────────────────────────────

uint32_t PatternSequencer::swingSamples() const
{
    if (!clock_ || !swing_.load(std::memory_order_relaxed)) return 0;
    const uint8_t per = swingPerBeat_.load(std::memory_order_relaxed);
    const uint32_t spb = clock_->samplesPerBeat();
    return per ? spb / per : 0;
}

uint32_t PatternSequencer::swung(uint32_t pos) const
{
    const uint32_t unit = swingSamples();
    if (unit < 2) return pos;
    /* Only every second subdivision moves: the pairs keep their first half
     * where it was, which is what makes it a feel and not a shift. */
    if (!((pos / unit) & 1u)) return pos;
    const uint32_t late = (uint32_t)((uint64_t)unit * swing_.load(std::memory_order_relaxed) / 100u);
    const uint32_t loop = clock_ ? clock_->loopSamples() : 0;
    uint32_t at = pos + late;
    /* A hit at the very end pushed past it comes round, where the next pass's
     * window picks it up -- it does not vanish for one lap. */
    if (loop && at >= loop) at -= loop;
    return at;
}

uint32_t PatternSequencer::unswung(uint32_t pos) const
{
    const uint32_t unit = swingSamples();
    if (unit < 2) return pos;
    const uint32_t late = (uint32_t)((uint64_t)unit * swing_.load(std::memory_order_relaxed) / 100u);
    /* Inside an odd subdivision and past where its hits were pushed to: that is
     * a hit aimed at the swung off-beat, so it goes back to the straight one. */
    if (((pos / unit) & 1u) && (pos % unit) >= late) return pos - late;
    return pos;
}

void PatternSequencer::fireWindow(uint32_t from, uint32_t to)
{
    if (!sink_ || from >= to) return;
    const uint32_t loop = clock_->loopSamples();
    for (uint16_t i = 0; i < counts_[activeSeq_]; ++i) {
        const Event& e = events_[activeSeq_][i];
        if (e.fresh) continue;
        /* Where it sounds, not where it is stored: the pattern stays straight
         * and the swing is applied on the way out. */
        const uint32_t at = swung(e.pos);
        if (at < from || at >= to) continue;
        if (e.pad >= 16) { emit(e.pad, e.vel, true); continue; }
        const uint16_t bit = (uint16_t)(1u << e.pad);
        /* Still held by the note before it: let go first, so every press the
         * sink sees has its own release. */
        if (gated_ & bit) {
            emit(e.pad, 0, false);
            gated_ &= (uint16_t)~bit;
        }
        emit(e.pad, e.vel, true);
        /* A tap (or a note whose pad is still down in the take) gets one poll
         * of gate; a held note keeps its pad down until its length runs out. */
        const uint32_t len = e.open ? 0 : e.len;
        if (!len || !loop) { pendingRelease_ |= bit; continue; }
        const uint32_t end = (uint32_t)(((uint64_t)at + len) % loop);
        if (end > at && end < to) {
            pendingRelease_ |= bit;     // over inside this poll
        } else {
            releaseAt_[e.pad] = end;
            gated_ |= bit;
        }
    }
}

void PatternSequencer::releaseWindow(uint32_t from, uint32_t to)
{
    if (!gated_ || from >= to) return;
    for (uint8_t pad = 0; pad < 16; ++pad) {
        const uint16_t bit = (uint16_t)(1u << pad);
        if (!(gated_ & bit) || releaseAt_[pad] < from || releaseAt_[pad] >= to) continue;
        emit(pad, 0, false);
        gated_ &= (uint16_t)~bit;
    }
}

void PatternSequencer::dropGates()
{
    pendingRelease_ |= gated_;
    gated_ = 0;
}

void PatternSequencer::emit(uint8_t pad, uint8_t vel, bool on)
{
    if (presses_) {
        if (pressCount_ < kMaxPresses) presses_[pressCount_++] = { pad, vel, on };
    } else if (sink_) {
        sink_(sinkCtx_, pad, vel, on);
    }
}

void PatternSequencer::poll()
{
    if (!clock_) return;
    uint16_t n = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(editMutex_);
        presses_ = pressBuf_;
        pressCount_ = 0;
        pollLocked();
        n = pressCount_;
        presses_ = nullptr;
    }
    if (!sink_) return;
    for (uint16_t i = 0; i < n; ++i) sink_(sinkCtx_, pressBuf_[i].pad, pressBuf_[i].vel, pressBuf_[i].on);
}

void PatternSequencer::pollLocked()
{

    /* Release what the last pass fired, before anything else. A pad left
     * pressed is not a silent bug: the LED stays lit and every "is this pad
     * pressed" reader answers yes for good -- which is what stopped the
     * sampler's waveform settling on one pad. */
    if (pendingRelease_ && sink_) {
        for (uint8_t pad = 0; pad < 16; ++pad) {
            if (pendingRelease_ & (1u << pad)) emit(pad, 0, false);
        }
        pendingRelease_ = 0;
    }

    const uint32_t loop = clock_->loopSamples();
    const uint32_t now  = clock_->positionSamples();

    /* The clock hands over from count-in to the loop on its own sample; this
     * only notices, before the hits are drained so the ones made just after
     * beat one are already part of a running take. */
    if (state_ == SequencerState::PreBeat) {
        if (!clock_->running()) {
            setTransport(SequencerMode::None, SequencerState::Idle);
        } else if (!clock_->countingIn()) {
            setTransport(mode_, SequencerState::Running);
            lastPos_ = preBeatPos_;
        }
    }
    clock_->takeCountInEnded();

    drainInput(loop);

    if (state_ != SequencerState::Running || !clock_->running()) {
        lastPos_ = now;
        return;
    }

    /* Half-open [from, to): every position is covered exactly once across
     * consecutive polls, so nothing fires twice and nothing is skipped. */
    if (now >= lastPos_) {
        releaseWindow(lastPos_, now);
        fireWindow(lastPos_, now);
    } else {
        releaseWindow(lastPos_, loop);
        fireWindow(lastPos_, loop);
        /* The loop has come round: what was recorded during the pass just
         * ended is now part of the pattern and plays from here on. */
        for (uint8_t s = 0; s < kNumSequences; ++s)
            for (uint16_t i = 0; i < counts_[s]; ++i) events_[s][i].fresh = 0;

        /* The only moment a queued scene may take over. Doing it here rather
         * than when the button was pressed is what keeps a swap on the bar
         * line instead of cutting the pattern in half. */
        if (songMode_.load(std::memory_order_relaxed)) {
            if (!songAdvance()) { lastPos_ = now; return; }
        } else if (pendingSeq_ != activeSeq_) {
            applyScene(pendingSeq_);
        }

        releaseWindow(0, now);
        fireWindow(0, now);

        if (!loop_) stop();
    }
    lastPos_ = now;
}

// ── Scenes ─────────────────────────────────────────────────────────────────

void PatternSequencer::applyScene(uint8_t seq, bool remember)
{
    if (seq >= kNumSequences) return;
    activeSeq_  = seq;
    pendingSeq_ = seq;
    /* A note held over the change was timed against the old loop. */
    dropGates();
    /* The scene's length is the loop, and the click counts that same loop --
     * handing it to the metronome is what keeps bar one and the downbeat the
     * same instant. */
    if (clock_) clock_->setBarCount(bars_[seq]);
    lastPos_ = 0;
    if (remember) markDirty();   // the file records which scene was left playing
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
        dropGates();
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
        events_[seq][n + i] = { e.pos + loop, e.len, e.pad, e.vel, 0, 0 };
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

// ── Song ───────────────────────────────────────────────────────────────────

bool PatternSequencer::songBlock(uint8_t i, uint8_t& scene, uint8_t& repeats) const
{
    if (i >= songLength()) return false;
    const uint16_t w = song_[i].load(std::memory_order_relaxed);
    scene   = (uint8_t)(w >> 8);
    repeats = (uint8_t)(w & 0xFF);
    return true;
}

bool PatternSequencer::songInsert(uint8_t i, uint8_t scene, uint8_t repeats)
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    const uint8_t n = songLength();
    if (n >= kSongMax || i > n || scene >= sceneCount_) return false;
    if (repeats < 1) repeats = 1;
    if (repeats > kSongMaxRepeats) repeats = kSongMaxRepeats;
    for (uint8_t b = n; b > i; --b) song_[b].store(song_[b - 1].load());
    song_[i].store(packBlock(scene, repeats));
    songLen_.store((uint8_t)(n + 1), std::memory_order_release);
    /* The block playing keeps playing: everything from it on moved one right. */
    if (songMode() && i <= songPos_.load()) songPos_.fetch_add(1);
    markDirty();
    return true;
}

bool PatternSequencer::songRemove(uint8_t i)
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    const uint8_t n = songLength();
    if (i >= n) return false;
    for (uint8_t b = i; b + 1 < n; ++b) song_[b].store(song_[b + 1].load());
    songLen_.store((uint8_t)(n - 1), std::memory_order_release);
    const uint8_t pos = songPos_.load();
    if (pos > i) songPos_.store((uint8_t)(pos - 1));
    markDirty();
    return true;
}

bool PatternSequencer::songSet(uint8_t i, uint8_t scene, uint8_t repeats)
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    if (i >= songLength() || scene >= sceneCount_) return false;
    if (repeats < 1) repeats = 1;
    if (repeats > kSongMaxRepeats) repeats = kSongMaxRepeats;
    song_[i].store(packBlock(scene, repeats));
    markDirty();
    return true;
}

void PatternSequencer::setSongMode(bool on)
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    songMode_.store(on, std::memory_order_relaxed);
    /* Switched on from a stop, a song starts at its top -- not wherever the
     * last run happened to be left. */
    const bool stopped = !(state_ == SequencerState::Running && clock_ && clock_->running());
    if (on && stopped && songLength()) songLocate(0);
}

void PatternSequencer::songLocate(uint8_t i)
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    if (i >= songLength()) return;
    if (state_ == SequencerState::Running && clock_ && clock_->running()) {
        songJump_.store(i);          // on the bar line, like a scene change
        return;
    }
    songPos_.store(i);
    songPass_.store(0);
    songJump_.store(-1);
    uint8_t scene = 0, reps = 0;
    songBlock(i, scene, reps);
    applyScene(scene);
    if (clock_) clock_->resetPosition();
}

bool PatternSequencer::songAdvance()
{
    const uint8_t n = songLength();
    if (!n) return true;             // nothing arranged: keep looping the scene

    uint8_t pos  = songPos_.load();
    uint8_t pass = (uint8_t)(songPass_.load() + 1);
    const int16_t jump = songJump_.exchange(-1);
    uint8_t scene = 0, reps = 1;

    if (jump >= 0 && jump < n) {
        pos = (uint8_t)jump;
        pass = 0;
    } else if (!songBlock(pos, scene, reps) || pass >= reps) {
        pass = 0;
        if (++pos >= n) {
            pos = 0;
            if (!songLoop_.load()) {
                stop();
                return false;
            }
        }
    }
    songPos_.store(pos);
    songPass_.store(pass);
    if (songBlock(pos, scene, reps) && scene < sceneCount_ && scene != activeSeq_) {
        applyScene(scene, /*remember=*/false);
    }
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
            if (!events_[s][i].open)
                events_[s][i].len = (uint32_t)(((uint64_t)events_[s][i].len * after) / before);
        }
    }
    for (uint8_t pad = 0; pad < 16; ++pad)
        releaseAt_[pad] = (uint32_t)(((uint64_t)releaseAt_[pad] * after) / before);
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
    /* The song follows: blocks of the scene that went are dropped, and the
     * ones after it point one lower, like the scenes themselves. */
    for (int b = (int)songLength() - 1; b >= 0; --b) {
        const uint16_t w = song_[b].load();
        const uint8_t sc = (uint8_t)(w >> 8);
        if (sc == seq)     songRemove((uint8_t)b);
        else if (sc > seq) song_[b].store(packBlock((uint8_t)(sc - 1), (uint8_t)(w & 0xFF)));
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
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    if (!clock_->running()) clock_->start();
    lastPos_ = clock_->positionSamples();
    setTransport(SequencerMode::Playback, SequencerState::Running);
}

void PatternSequencer::playbackAt(uint32_t pos, uint8_t countInBeats)
{
    if (!clock_) return;
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    if (clock_->running()) stop();
    if (countInBeats) {
        clock_->startCountIn(countInBeats, pos);
        preBeatPos_ = lastPos_ = clock_->positionSamples();
        setTransport(SequencerMode::Playback, SequencerState::PreBeat);
        return;
    }
    clock_->startAt(pos);
    lastPos_ = clock_->positionSamples();
    setTransport(SequencerMode::Playback, SequencerState::Running);
}

void PatternSequencer::startTake(SequencerMode m)
{
    /* Already counting in: the take is still waiting for its beat one. */
    if (state_ == SequencerState::PreBeat) { setTransport(m, SequencerState::PreBeat); return; }
    const uint8_t beats = clock_->running() ? 0 : countInBeats();
    if (beats) {
        clock_->startCountIn(beats);
        preBeatPos_ = lastPos_ = 0;
        setTransport(m, SequencerState::PreBeat);
        return;
    }
    if (!clock_->running()) clock_->start();
    lastPos_ = clock_->positionSamples();
    setTransport(m, SequencerState::Running);
}

void PatternSequencer::record()
{
    if (!clock_) return;
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    /* Record replaces; overdub adds. Clearing here rather than at the wrap is
     * what makes the button honest -- you hear the old take stop immediately. */
    counts_[activeSeq_] = 0;
    markDirty();
    startTake(SequencerMode::Record);
}

void PatternSequencer::overdub()
{
    if (!clock_) return;
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    startTake(SequencerMode::Overdub);
}

void PatternSequencer::stop()
{
    /* Called from the UI, the CDC loop and the song worker while the poll
     * thread fires the pattern: the held pads are read and cleared under the
     * scene lock, and released after it, like poll() does -- a release lost
     * to a pass in flight leaves the pad held for good. Inside poll() (a song
     * that ends) the releases join the pass's own. */
    uint16_t held = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(editMutex_);
        /* A stopped song is over; the next plain Play plays the scene, and
         * song mode switched back on starts from the top. */
        songMode_.store(false, std::memory_order_relaxed);
        songPos_.store(0);
        songPass_.store(0);
        songJump_.store(-1);
        /* A take that ends with pads down: their notes end here. */
        if (clock_) closeAllNotes(clock_->elapsedSamples(), clock_->loopSamples());
        held = (uint16_t)(pendingRelease_ | gated_);
        pendingRelease_ = 0;
        gated_ = 0;
        if (presses_) {
            for (uint8_t pad = 0; pad < 16; ++pad) {
                if (held & (1u << pad)) emit(pad, 0, false);
            }
            held = 0;
        }
        if (clock_) clock_->stop();
        setTransport(SequencerMode::None, SequencerState::Idle);
    }
    /* Whatever is still held goes down with the transport, or it stays held
     * until the next pass that never comes. */
    if (!sink_) return;
    for (uint8_t pad = 0; pad < 16; ++pad) {
        if (held & (1u << pad)) sink_(sinkCtx_, pad, 0, false);
    }
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
    songLen_.store(0, std::memory_order_release);
    songMode_.store(false);
    songLoop_.store(true);
    songPos_.store(0);
    songPass_.store(0);
    songJump_.store(-1);
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

float PatternSequencer::sceneEventLength(uint8_t seq, uint16_t i) const
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    if (seq >= kNumSequences || i >= counts_[seq] || !clock_) return 0.0f;
    const uint32_t loop = (uint32_t)clock_->samplesPerBeat() * clock_->beatsPerBar() * bars_[seq];
    const Event& e = events_[seq][i];
    if (!loop) return 0.0f;
    if (!e.open) return (float)e.len / (float)loop;
    /* The pad is still down: as long as it has been held so far, so the note
     * grows under the finger instead of appearing whole on the release. */
    uint32_t len = clock_->elapsedSamples() - openAt_[e.pad];
    if (len > loop) len = loop;
    return (float)len / (float)loop;
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

bool PatternSequencer::addEventTicks(uint8_t seq, uint32_t ticks, uint8_t pad, uint8_t vel,
                                     uint32_t lenTicks)
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    if (seq >= kNumSequences || !clock_ || counts_[seq] >= kMaxEvents) return false;
    const uint32_t spb = clock_->samplesPerBeat();
    const uint32_t pos = (uint32_t)(((uint64_t)ticks * spb) / kPpq);
    /* A file written for a longer scene must not smuggle events past this
     * scene's end, where they would never play and would confuse the count. */
    const uint32_t loop = spb * clock_->beatsPerBar() * bars_[seq];
    if (loop && pos >= loop) return false;
    uint32_t len = (uint32_t)(((uint64_t)lenTicks * spb) / kPpq);
    if (loop && len > loop) len = loop;

    events_[seq][counts_[seq]++] = { pos, len, pad, vel, 0, 0 };
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

uint32_t PatternSequencer::eventLengthTicks(uint8_t seq, uint16_t i) const
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    if (seq >= kNumSequences || i >= counts_[seq] || !clock_) return 0;
    const uint32_t spb = clock_->samplesPerBeat();
    const Event& e = events_[seq][i];
    if (!spb || e.open) return 0;
    return (uint32_t)(((uint64_t)e.len * kPpq + spb / 2) / spb);
}

} // namespace crosspad

// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/sequencer/PatternSequencer.hpp"

#include <algorithm>

namespace crosspad {

// ── Input ──────────────────────────────────────────────────────────────────

void PatternSequencer::noteIn(uint8_t pad, uint8_t velocity)
{
    /* What the hit sounds as is asked now, on the pad's thread, before the pad
     * logic plays it: under 16 levels that is the source pad at a level, and
     * by the time poll() drains the ring the mode may have changed. */
    PadPerformance perf;
    if (perfSource_) perf = perfSource_(perfCtx_, pad, velocity);
    /* A press with no force is not one: 0 is how the ring says release. */
    stamp(pad, velocity ? velocity : 1, perf.pack());
}

void PatternSequencer::noteOut(uint8_t pad)
{
    stamp(pad, 0);
}

void PatternSequencer::stamp(uint8_t pad, uint8_t velocity, uint8_t perf)
{
    if (!clock_) return;
    const uint32_t head = ringHead_.load(std::memory_order_relaxed);
    const uint32_t next = (head + 1) % kInputRing;
    /* Full means poll() has stalled; drop rather than block the pad thread.
     * A dropped hit is a missing note in the take, which is recoverable --
     * a pad that waits on a mutex is a missed beat, which is not. */
    if (next == ringTail_.load(std::memory_order_acquire)) return;

    const uint32_t early = clock_->countInSamplesLeft();
    ring_[head] = { early ? 0u : clock_->positionSamples(), early, pad, velocity, perf,
                    clock_->elapsedSamples() };
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

void PatternSequencer::addEvent(uint8_t seq, uint32_t pos, uint8_t pad, uint8_t vel, uint8_t perf)
{
    if (seq >= kNumSequences || counts_[seq] >= kMaxEvents) return;
    events_[seq][counts_[seq]++] = { pos, pad, vel, 1, perf, 0 };
    markDirty();
}

void PatternSequencer::closeNote(const Hit& h, uint32_t loop)
{
    if (h.pad >= 16 || !open_[h.pad].on) return;
    Open& o = open_[h.pad];
    o.on = false;
    /* Let go before beat one: the press counted as beat one, the note is a
     * hit. Held longer than the loop is as long as the loop can say. */
    if (h.early || !loop || o.seq >= kNumSequences) return;
    /* Measured on the unwrapped count (as main's recorder did, bc0789a): a
     * hold over the loop end -- or over several -- is not read back as a
     * short one. A note cannot outlast the loop it plays in. */
    uint32_t len = h.elapsed - o.at;
    if (len > loop) len = loop;
    if (!len) return;
    for (uint16_t i = 0; i < counts_[o.seq]; ++i) {
        Event& e = events_[o.seq][i];
        if (e.pad == h.pad && e.pos == o.pos && !e.len) {
            e.len = len;
            markDirty();
            return;
        }
    }
}

void PatternSequencer::closeOpenNotes()
{
    if (!clock_) return;
    const uint32_t loop = clock_->loopSamples();
    const uint32_t now = clock_->positionSamples();
    const uint32_t elapsed = clock_->elapsedSamples();
    for (uint8_t pad = 0; pad < 16; ++pad) closeNote(Hit{now, 0, pad, 0, 0, elapsed}, loop);
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

    /* A press recorded is held open until its release comes through. */
    auto open = [this](uint8_t pad, uint32_t pos, uint32_t start, uint32_t at) {
        if (pad < 16) open_[pad] = Open{start, pos, activeSeq_, true, at};
    };
    /* A take onto the song keeps the hits where the song is, not in a scene. */
    const bool toSong = songTaking_.load(std::memory_order_relaxed) &&
                        (state_ == SequencerState::Running || state_ == SequencerState::PreBeat);
    while (tail != head) {
        const Hit& h = ring_[tail];
        /* A release ends the note its press opened, whatever the transport
         * is doing by now: a punch-out between the two still keeps the note. */
        if (!h.vel) {
            closeNote(h, loop);
            if (songTaking_.load(std::memory_order_relaxed)) takeSongRelease(h, loop);
        } else if (toSong) {
            if (!h.early || h.early <= grace) takeSongHit(h, loop);
        /* Erase held: the hit names a pad to take out, it is not a note. */
        } else if (erasing) {
            erasePad(activeSeq_, h.pad);
        } else if (taking && h.early) {
            /* Made during the count-in: a hair early for the take's first
             * beat is that beat -- the player aimed at it -- anything earlier
             * was not part of the take. Held from beat one. */
            if (h.early <= grace) {
                const uint32_t at = quantise(preBeatPos_, loop);
                addEvent(activeSeq_, at, h.pad, h.vel, h.perf);
                open(h.pad, at, preBeatPos_, h.elapsed + h.early);
            }
        } else if (taking && state_ == SequencerState::Running) {
            /* Into the scene that is sounding: you record into what you hear --
             * including the swing, so the heard position comes back to the
             * straight grid before it is snapped to it. The length is what the
             * finger held, from where it went down, quantised or not. */
            const uint32_t at = quantise(unswung(h.pos, activeSeq_), loop);
            addEvent(activeSeq_, at, h.pad, h.vel, h.perf);
            open(h.pad, at, h.pos, h.elapsed);
        }
        tail = (tail + 1) % kInputRing;
    }
    ringTail_.store(tail, std::memory_order_release);
}

// ── Playback ───────────────────────────────────────────────────────────────

void PatternSequencer::setSceneSwing(uint8_t seq, uint8_t percent, uint8_t perBeat)
{
    if (seq >= kNumSequences) return;
    if (percent > kMaxSwing) percent = kMaxSwing;
    const bool sixteenths = perBeat == 4 || (perBeat != 2 && swingSixteenths_[seq].load(std::memory_order_relaxed));
    if (percent == sceneSwing(seq) && sixteenths == swingSixteenths_[seq].load(std::memory_order_relaxed)) return;
    swing_[seq].store(percent, std::memory_order_relaxed);
    swingSixteenths_[seq].store(sixteenths, std::memory_order_relaxed);
    markDirty();
}

uint32_t PatternSequencer::swingSamples(uint8_t seq) const
{
    if (!clock_ || !sceneSwing(seq)) return 0;
    return clock_->samplesPerBeat() / sceneSwingPerBeat(seq);
}

uint32_t PatternSequencer::swingLate(uint32_t unit, uint8_t seq) const
{
    /* Intensity 0..100 over half a subdivision: 100 puts the off-beat three
     * quarters of the way through the pair, which is where a sampler's swing
     * knob stops at 75 %. The triplet feel is two thirds of the way, which
     * falls at 67. */
    return (uint32_t)((uint64_t)unit * sceneSwing(seq) / 200u);
}

uint32_t PatternSequencer::swung(uint32_t pos, uint32_t loop, uint8_t seq) const
{
    const uint32_t unit = swingSamples(seq);
    if (unit < 2) return pos;
    /* Only every second subdivision moves: the pairs keep their first half
     * where it was, which is what makes it a feel and not a shift. */
    if (!((pos / unit) & 1u)) return pos;
    const uint32_t late = swingLate(unit, seq);
    uint32_t at = pos + late;
    /* A hit at the very end pushed past it comes round, where the next pass's
     * window picks it up -- it does not vanish for one lap. */
    if (loop && at >= loop) at -= loop;
    return at;
}

uint32_t PatternSequencer::unswung(uint32_t pos, uint8_t seq) const
{
    const uint32_t unit = swingSamples(seq);
    if (unit < 2) return pos;
    const uint32_t late = swingLate(unit, seq);
    /* Inside an odd subdivision and past where its hits were pushed to: that is
     * a hit aimed at the swung off-beat, so it goes back to the straight one. */
    if (((pos / unit) & 1u) && (pos % unit) >= late) return pos - late;
    return pos;
}

void PatternSequencer::fireWindow(uint32_t from, uint32_t to, uint8_t route)
{
    if (!hasSink() || from >= to) return;
    for (uint16_t i = 0; i < counts_[activeSeq_]; ++i) {
        const Event& e = events_[activeSeq_][i];
        if (e.fresh) continue;
        /* Where it sounds, not where it is stored: the pattern stays straight
         * and the swing is applied on the way out. */
        const uint32_t at = swung(e.pos, activeSeq_);
        if (at < from || at >= to) continue;
        strike(route, e.pad, e.vel, e.perf, e.len, to - at);
    }
}

void PatternSequencer::strike(uint8_t route, uint8_t pad, uint8_t vel, uint8_t perf, uint32_t len,
                              uint32_t gone)
{
    /* Held per route: pad 3 of one kit holding says nothing about pad 3 of
     * the other. */
    const uint8_t slot = (uint8_t)(route * 16 + pad);
    const uint64_t bit = pad < 16 && route < kRoutes ? (1ull << slot) : 0;
    /* A pad a note still holds lets go before it is struck again. */
    if (held_ & bit) {
        emit(route, pad, 0, false);
        held_ &= ~bit;
    }
    emit(route, pad, vel, true, perf);
    if (!bit) return;
    if (len > gone) {
        /* Held for as long as it was played: a chord, a sample on HOLD.
         * Counted from where it sounds: the rest of this window is already
         * part of its length, the next poll's elapsed is not. */
        held_ |= bit;
        relLeft_[slot] = len - gone;
        pendingRelease_ &= ~bit;
    } else {
        pendingRelease_ |= bit;   // a hit, or over inside this poll
    }
}

void PatternSequencer::fireNotes(const std::vector<SongNote>& notes, uint32_t loop,
                                 uint32_t from, uint32_t to, uint8_t route, uint8_t swingScene)
{
    if (!hasSink() || from >= to) return;
    const uint8_t feel = swingScene < kNumSequences ? swingScene : activeSeq_;
    for (const SongNote& n : notes) {
        const uint32_t at = swung(n.pos, loop, feel);
        if (at < from || at >= to) continue;
        strike(route, n.pad, n.vel, n.perf, n.len, to - at);
    }
}

void PatternSequencer::releaseHeld(uint32_t samples)
{
    if (!held_) return;
    for (uint8_t slot = 0; slot < kRoutes * 16; ++slot) {
        const uint64_t bit = 1ull << slot;
        if (!(held_ & bit)) continue;
        if (relLeft_[slot] > samples) {
            relLeft_[slot] -= samples;
            continue;
        }
        emit(slot / 16, slot % 16, 0, false);
        held_ &= ~bit;
    }
}

void PatternSequencer::eraseWindow(uint32_t from, uint32_t to)
{
    if (from >= to || activeSeq_ >= kNumSequences) return;
    uint16_t kept = 0;
    for (uint16_t i = 0; i < counts_[activeSeq_]; ++i) {
        const Event& e = events_[activeSeq_][i];
        const uint32_t at = swung(e.pos, activeSeq_);
        if (!e.fresh && at >= from && at < to) continue;
        events_[activeSeq_][kept++] = e;
    }
    if (kept == counts_[activeSeq_]) return;
    counts_[activeSeq_] = kept;
    markDirty();
}

void PatternSequencer::emit(uint8_t route, uint8_t pad, uint8_t vel, bool on, uint8_t perf)
{
    if (presses_) {
        if (pressCount_ < kMaxPresses) presses_[pressCount_++] = { pad, vel, on, perf, route };
    } else {
        sinkPress(route, pad, vel, on, perf);
    }
}

void PatternSequencer::sinkPress(uint8_t route, uint8_t pad, uint8_t vel, bool on, uint8_t perf)
{
    if (routedSink_) {
        routedSink_(sinkCtx_, route, pad, vel, on, PadPerformance::unpack(perf));
        return;
    }
    /* A sink that knows only the pads plays route 0; another kit's notes
     * have nothing here to sound through. */
    if (route) return;
    if (performedSink_)  performedSink_(sinkCtx_, pad, vel, on, PadPerformance::unpack(perf));
    else if (sink_)      sink_(sinkCtx_, pad, vel, on);
}

void PatternSequencer::poll()
{
    if (!clock_) return;
    Press* const presses = pressBuf_;
    uint16_t n = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(editMutex_);
        presses_ = presses;
        pressCount_ = 0;
        pollLocked();
        n = pressCount_;
        presses_ = nullptr;
    }
    for (uint16_t i = 0; i < n; ++i)
        sinkPress(presses[i].route, presses[i].pad, presses[i].vel, presses[i].on, presses[i].perf);
}

void PatternSequencer::pollLocked()
{

    /* Release what the last pass fired, before anything else. A pad left
     * pressed is not a silent bug: the LED stays lit and every "is this pad
     * pressed" reader answers yes for good -- which is what stopped the
     * sampler's waveform settling on one pad. */
    if (pendingRelease_ && hasSink()) {
        for (uint8_t slot = 0; slot < kRoutes * 16; ++slot) {
            if (pendingRelease_ & (1ull << slot)) emit(slot / 16, slot % 16, 0, false);
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
     * consecutive polls, so nothing fires twice and nothing is skipped. A
     * Record take takes out what it passes over before it would sound. */
    const bool replacing = mode_ == SequencerMode::Record;
    const uint32_t elapsed = now >= lastPos_ ? now - lastPos_ : loop - lastPos_ + now;
    /* The notes due to end in this window let go before the next ones are
     * struck: one ending where another starts is two notes, not a tie. */
    releaseHeld(elapsed);
    /* Laid on a song cut into blocks, what sounds is the song's, counted by
     * the same clock: the scene's own pass below only keeps the books. */
    if (mapOn_) fireSong(elapsed, loop);
    if (now >= lastPos_) {
        if (replacing) eraseWindow(lastPos_, now);
        if (!mapOn_) fireWindow(lastPos_, now, sceneRoute());
    } else {
        if (replacing) eraseWindow(lastPos_, loop);
        if (!mapOn_) fireWindow(lastPos_, loop, sceneRoute());
        /* The loop has come round: what was recorded during the pass just
         * ended is now part of the pattern and plays from here on. */
        for (uint8_t s = 0; s < kNumSequences; ++s)
            for (uint16_t i = 0; i < counts_[s]; ++i) events_[s][i].fresh = 0;
        /* One pass replaces; the next ones layer on it, as on a hardware groovebox -- a
         * second Record pass would erase the take just played in. */
        if (replacing) setTransport(SequencerMode::Overdub, SequencerState::Running);

        /* The only moment a queued scene may take over. Doing it here rather
         * than when the button was pressed is what keeps a swap on the bar
         * line instead of cutting the pattern in half. */
        if (songMode_.load(std::memory_order_relaxed)) {
            if (!songAdvance()) { lastPos_ = now; return; }
        } else if (pendingSeq_ != activeSeq_) {
            applyScene(pendingSeq_);
        }

        if (!mapOn_) fireWindow(0, now, sceneRoute());

        if (!loop_) stop();
    }
    lastPos_ = now;
}

// ── Laid on a song's timeline ──────────────────────────────────────────────

void PatternSequencer::setSongMap(const std::vector<SongSpan>& spans, uint32_t wrapFrom, uint32_t wrapTo)
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    map_ = spans;
    mapLater_.clear();
    laterLeft_ = 0;
    mapWrapFrom_ = wrapFrom;
    mapWrapTo_ = wrapTo < wrapFrom ? wrapTo : 0;
    mapOn_ = true;
}

void PatternSequencer::setSongMapThen(const std::vector<SongSpan>& now, std::vector<SongSpan> later,
                                      uint32_t afterSamples, uint32_t wrapFrom, uint32_t wrapTo)
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    setSongMap(afterSamples ? now : later, wrapFrom, wrapTo);
    if (!afterSamples) return;
    mapLater_ = std::move(later);
    laterLeft_ = afterSamples;
}

uint32_t PatternSequencer::songMapLaterIn() const
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    return laterLeft_;
}

void PatternSequencer::clearSongMap()
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    map_.clear();
    mapLater_.clear();
    laterLeft_ = 0;
    mapOn_ = false;
}

/* What is due, up to where a later map takes over, then the rest on it. */
void PatternSequencer::fireSong(uint32_t samples, uint32_t loop)
{
    while (laterLeft_ && samples) {
        const uint32_t run = std::min(samples, laterLeft_);
        fireSongRun(run, loop);
        samples -= run;
        laterLeft_ -= run;
        if (!laterLeft_) {
            map_.swap(mapLater_);
            mapLater_.clear();
        }
    }
    if (samples) fireSongRun(samples, loop);
}

void PatternSequencer::fireSongRun(uint32_t samples, uint32_t loop)
{
    if (!loop) return;
    uint32_t pos = mapPos_;
    /* Past the wrap already: a take ran the song on past its blocks and
     * ended there, and the song came back shorter. The song's own ring
     * wraps such a position the same way (wrap_position()); a map that did
     * not went on past every block, silent until the next stop. */
    if (mapWrapFrom_ > mapWrapTo_ && pos >= mapWrapFrom_)
        pos = mapWrapTo_ + (pos - mapWrapFrom_) % (mapWrapFrom_ - mapWrapTo_);
    const uint8_t muted = mapMuted_.load(std::memory_order_relaxed);
    while (samples) {
        /* Up to the song's wrap, then on from where it wraps to. */
        const bool wraps = mapWrapFrom_ > pos;
        const uint32_t step = wraps ? std::min(samples, mapWrapFrom_ - pos) : samples;
        const uint32_t end = pos + step;
        for (const SongSpan& s : map_) {
            if (muted & (1u << s.route)) continue;
            const uint32_t x = std::max(pos, s.a), y = std::min(end, s.b);
            if (x >= y) continue;
            /* A block's own notes go round their own length; the scene's
             * round the scene, which is the clock's loop. */
            const uint32_t len = s.notes ? s.loop : loop;
            if (!len) continue;
            uint32_t at = (x - s.a + s.scene) % len;
            uint32_t left = y - x;
            while (left) {
                const uint32_t run = std::min(left, len - at);
                if (s.notes) fireNotes(*s.notes, len, at, at + run, s.route, s.swingScene);
                else         fireWindow(at, at + run, s.route);
                left -= run;
                at = 0;
            }
        }
        pos = end;
        samples -= step;
        if (wraps && pos >= mapWrapFrom_) {
            /* A take going round a loop region: what is held ends at the
             * region's end, and the next lap is a pass of its own. */
            if (songTaking_.load(std::memory_order_relaxed)) {
                closeSongHeld(mapWrapFrom_);
                takeLap_.fetch_add(1, std::memory_order_release);
            }
            pos = mapWrapTo_;
        }
    }
    mapPos_ = pos;
}

uint32_t PatternSequencer::songMapPosition() const
{
    return mapPos_.load(std::memory_order_relaxed);
}

// ── A take onto the song ───────────────────────────────────────────────────

bool PatternSequencer::beginSongTake(size_t capacity, uint8_t scene)
{
    std::vector<SongTakeNote> room;
    room.reserve(capacity);
    if (room.capacity() < capacity) return false;
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    take_.swap(room);
    take_.clear();
    takeCap_ = capacity;
    takeScene_ = scene < kNumSequences ? scene : activeSeq_;
    for (auto& o : takeOpen_) o.on = false;
    takeLap_.store(0, std::memory_order_relaxed);
    songTaking_.store(true, std::memory_order_release);
    return true;
}

void PatternSequencer::takeSongNotes(std::vector<SongTakeNote>& out, bool closeHeld)
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    if (closeHeld) closeSongHeld(mapPos_.load(std::memory_order_relaxed));
    out.insert(out.end(), take_.begin(), take_.end());
    take_.clear();
}

void PatternSequencer::endSongTake()
{
    std::vector<SongTakeNote> gone;
    {
        std::lock_guard<std::recursive_mutex> lock(editMutex_);
        songTaking_.store(false, std::memory_order_release);
        take_.swap(gone);
        takeCap_ = 0;
        for (auto& o : takeOpen_) o.on = false;
    }
}

uint32_t PatternSequencer::songPosOf(const Hit& h, uint32_t loop, uint16_t& lap) const
{
    /* The map stands where the song was at the last poll, and the clock where
     * lastPos_ says: the hit is that far past both. */
    const uint32_t last = loop ? lastPos_ % loop : lastPos_;
    const uint32_t past = loop ? (h.pos + loop - last) % loop : h.pos - last;
    uint32_t pos = mapPos_.load(std::memory_order_relaxed);
    lap = takeLap_.load(std::memory_order_relaxed);
    /* Stamped just before the last poll read the clock and drained just
     * after it: a hair behind, not a whole loop ahead. */
    if (loop && past > loop / 2) return pos > loop - past ? pos - (loop - past) : 0;
    pos += past;
    if (mapWrapFrom_ && pos >= mapWrapFrom_) {
        pos = mapWrapTo_ + (pos - mapWrapFrom_);
        ++lap;
    }
    return pos;
}

void PatternSequencer::takeSongHit(const Hit& h, uint32_t loop)
{
    if (h.pad >= 16) return;
    uint16_t lap = 0;
    /* A hair early for the take's beat one is that beat, as in a scene; it
     * is struck where the song starts. */
    const uint32_t raw = h.early ? mapPos_.load(std::memory_order_relaxed) : songPosOf(h, loop, lap);
    if (h.early) lap = takeLap_.load(std::memory_order_relaxed);
    /* The song's grid, not the scene's loop: nothing wraps a take. */
    const uint32_t at = quantise(h.early ? raw : unswung(raw, takeScene_), 0);
    TakeOpen& o = takeOpen_[h.pad];
    /* Struck again before its release came through: the first is a hit. */
    if (o.on && take_.size() < takeCap_) take_.push_back({o.pos, 0, h.pad, o.vel, o.perf, o.lap});
    o = TakeOpen{at, raw, h.vel, h.perf, lap, true};
}

void PatternSequencer::takeSongRelease(const Hit& h, uint32_t loop)
{
    if (h.pad >= 16 || !takeOpen_[h.pad].on) return;
    TakeOpen& o = takeOpen_[h.pad];
    o.on = false;
    uint16_t lap = 0;
    const uint32_t now = songPosOf(h, loop, lap);
    /* A release in another lap was ended at the wrap already. */
    const uint32_t len = (lap == o.lap && now > o.start) ? now - o.start : 0;
    if (take_.size() < takeCap_) take_.push_back({o.pos, len, h.pad, o.vel, o.perf, o.lap});
}

void PatternSequencer::closeSongHeld(uint32_t at)
{
    for (uint8_t pad = 0; pad < 16; ++pad) {
        TakeOpen& o = takeOpen_[pad];
        if (!o.on) continue;
        o.on = false;
        const uint32_t len = at > o.start ? at - o.start : 0;
        if (take_.size() < takeCap_) take_.push_back({o.pos, len, pad, o.vel, o.perf, o.lap});
    }
}

// ── Scenes ─────────────────────────────────────────────────────────────────

void PatternSequencer::applyScene(uint8_t seq, bool remember)
{
    if (seq >= kNumSequences) return;
    activeSeq_  = seq;
    pendingSeq_ = seq;
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
        events_[seq][n + i] = { e.pos + loop, e.pad, e.vel, 0, e.perf, e.len };
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
            events_[s][i].len = (uint32_t)(((uint64_t)events_[s][i].len * after) / before);
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
    swing_[idx].store(0, std::memory_order_relaxed);
    swingSixteenths_[idx].store(false, std::memory_order_relaxed);
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
        swing_[s].store(swing_[s + 1].load(std::memory_order_relaxed), std::memory_order_relaxed);
        swingSixteenths_[s].store(swingSixteenths_[s + 1].load(std::memory_order_relaxed), std::memory_order_relaxed);
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
    swing_[last].store(0, std::memory_order_relaxed);
    swingSixteenths_[last].store(false, std::memory_order_relaxed);
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
    /* Already running -- out of a take: REC or DUB pressed again, or PLAY over
     * an overdub. Only the mode changes. The window the next poll fires and
     * where the song map stands carry on as they were: taken back to now and
     * to the song's bar one, a note due in the last few milliseconds was
     * skipped and a seq track's blocks jumped to the top of the song. */
    if (clock_->running()) {
        setTransport(SequencerMode::Playback, SequencerState::Running);
        return;
    }
    /* From where it stopped: the clock holds its position through a stop,
     * and resetClick() is what takes it back to bar one. */
    clock_->startAt(clock_->positionSamples());
    lastPos_ = clock_->positionSamples();
    mapPos_ = 0;
    setTransport(SequencerMode::Playback, SequencerState::Running);
}

void PatternSequencer::playbackAt(uint32_t pos, uint8_t countInBeats)
{
    if (!clock_) return;
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    if (clock_->running()) stop();
    mapPos_ = pos;
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
    const uint32_t held = clock_->positionSamples();
    if (beats) {
        clock_->startCountIn(beats, held);
        preBeatPos_ = lastPos_ = held;
        setTransport(m, SequencerState::PreBeat);
        return;
    }
    /* Into a running loop the window the next poll fires carries on, as in
     * playback(): set to now, a note due since the last poll was skipped at
     * the very press that punched in. */
    if (!clock_->running()) {
        clock_->startAt(held);
        lastPos_ = clock_->positionSamples();
    }
    setTransport(m, SequencerState::Running);
}

void PatternSequencer::record()
{
    if (!clock_) return;
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    /* Record replaces, overdub adds -- but nothing is taken out at the press:
     * a take cancelled during its count-in must leave the scene as it was.
     * The old notes go as the head passes over them (pollLocked). */
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
    uint64_t held = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(editMutex_);
        /* A stopped song is over; the next plain Play plays the scene, and
         * song mode switched back on starts from the top. */
        songMode_.store(false, std::memory_order_relaxed);
        songPos_.store(0);
        songPass_.store(0);
        songJump_.store(-1);
        /* A note being recorded ends here; one being played lets go. */
        closeOpenNotes();
        /* The pass is over: what it recorded plays from the next start on,
         * not only from that start's first wrap. */
        for (uint8_t s = 0; s < kNumSequences; ++s)
            for (uint16_t i = 0; i < counts_[s]; ++i) events_[s][i].fresh = 0;
        held = pendingRelease_ | held_;
        pendingRelease_ = 0;
        held_ = 0;
        if (presses_) {
            for (uint8_t slot = 0; slot < kRoutes * 16; ++slot) {
                if (held & (1ull << slot)) emit(slot / 16, slot % 16, 0, false);
            }
            held = 0;
        }
        if (clock_) clock_->stop();
        setTransport(SequencerMode::None, SequencerState::Idle);
    }
    /* Whatever is still held goes down with the transport, or it stays held
     * until the next pass that never comes. */
    for (uint8_t slot = 0; slot < kRoutes * 16; ++slot) {
        if (held & (1ull << slot)) sinkPress(slot / 16, slot % 16, 0, false, 0);
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
        swing_[s].store(0, std::memory_order_relaxed);
        swingSixteenths_[s].store(false, std::memory_order_relaxed);
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

bool PatternSequencer::addEventTicks(uint8_t seq, uint32_t ticks, uint8_t pad, uint8_t vel, uint32_t lenTicks,
                                     PadPerformance performance)
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    if (seq >= kNumSequences || !clock_ || counts_[seq] >= kMaxEvents) return false;
    const uint32_t spb = clock_->samplesPerBeat();
    const uint32_t pos = (uint32_t)(((uint64_t)ticks * spb) / kPpq);
    /* A file written for a longer scene must not smuggle events past this
     * scene's end, where they would never play and would confuse the count. */
    const uint32_t loop = spb * clock_->beatsPerBar() * bars_[seq];
    if (loop && pos >= loop) return false;

    const uint32_t len = (uint32_t)(((uint64_t)lenTicks * spb) / kPpq);
    events_[seq][counts_[seq]++] = { pos, pad, vel, 0, performance.pack(), len };
    return true;
}

PadPerformance PatternSequencer::eventPerformance(uint8_t seq, uint16_t i) const
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    if (seq >= kNumSequences || i >= counts_[seq]) return {};
    return PadPerformance::unpack(events_[seq][i].perf);
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
    if (!spb) return 0;
    const uint32_t len = events_[seq][i].len;
    /* Never rounded to nothing: a note held a hair is still a note. */
    return len ? std::max<uint32_t>(1, (uint32_t)(((uint64_t)len * kPpq + spb / 2) / spb)) : 0;
}

float PatternSequencer::sceneEventLength(uint8_t seq, uint16_t i) const
{
    std::lock_guard<std::recursive_mutex> lock(editMutex_);
    if (seq >= kNumSequences || i >= counts_[seq] || !clock_) return 0.0f;
    const uint32_t loop = (uint32_t)clock_->samplesPerBeat() * clock_->beatsPerBar() * bars_[seq];
    if (!loop) return 0.0f;
    const Event& e = events_[seq][i];
    uint32_t len = e.len;
    /* A note whose pad is still down in a take: as long as it has been held
     * so far, so the UI draws it growing (main's bc0789a). */
    if (!len && e.pad < 16) {
        const Open& o = open_[e.pad];
        if (o.on && o.seq == seq && o.pos == e.pos) {
            const uint32_t now = clock_->elapsedSamples();
            len = now > o.at ? now - o.at : 0;
            if (len > loop) len = loop;
        }
    }
    return (float)len / (float)loop;
}

} // namespace crosspad

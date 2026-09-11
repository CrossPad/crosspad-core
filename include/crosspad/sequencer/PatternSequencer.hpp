// SPDX-License-Identifier: MIT
#pragma once

#include "crosspad/sequencer/ISequencerEngine.hpp"
#include "crosspad/sequencer/MetronomeNode.hpp"
#include "crosspad/sequencer/SequencerController.hpp"
#include "crosspad/sequencer/SequencerTypes.hpp"

#include <atomic>
#include <cstdint>

namespace crosspad {

/// A loop recorder for the pads, on the metronome's clock.
///
/// This is the portable software `ISequencerEngine` the interface always
/// anticipated -- the Arduino build wraps ML_MidiSeq, this one needs no engine
/// underneath because MetronomeNode already counts samples in the render. Both
/// the click a player follows and the grid their take is quantised to are then
/// read from the same counter, so a hit cannot land on the grid and off the
/// click at once.
///
/// Positions are samples from the loop start, and the loop is the metronome's
/// own (`bars x beatsPerBar`). Changing the bar count therefore changes the
/// length of the pattern and the click together, which is what "set the number
/// of bars" has to mean for the two to stay locked.
///
/// Threading, which is the whole reason for the ring below: hits arrive on
/// whichever thread runs the pads and must be stamped *there*, at the moment
/// they happen, or the timestamp inherits the queue latency it was supposed to
/// measure. Playback runs on whatever thread calls poll(). So noteIn() only
/// pushes into a single-producer/single-consumer ring and poll() drains it --
/// no lock is taken on either side, and neither can block the other.
class PatternSequencer : public ISequencerEngine {
public:
    /// Fired for every note due, from the thread that calls poll().
    using NoteSink = void (*)(void* ctx, uint8_t pad, uint8_t velocity);

    static constexpr uint8_t  kNumSequences = SequencerLimits::MAX_SEQ_INDEX + 1;
    static constexpr uint16_t kMaxEvents    = 192;   ///< per sequence
    static constexpr uint8_t  kInputRing    = 32;    ///< hits in flight

    void attach(MetronomeNode* clock) { clock_ = clock; }
    /// The controller mirrors transport state for the UI. It is the caller's
    /// half of the contract the interface already assumes: the engine reports
    /// where it got to, the controller holds it.
    void setController(SequencerController* c) { controller_ = c; }
    void setSink(NoteSink fn, void* ctx) { sink_ = fn; sinkCtx_ = ctx; }

    // ── Input, called from the pad thread ─────────────────────────────────

    /// Stamp a hit with the clock position it happened at. Cheap and wait-free;
    /// safe to call when nothing is recording (it is dropped in poll()).
    void noteIn(uint8_t pad, uint8_t velocity);

    // ── Driven from a timer or the audio task ─────────────────────────────

    /// Fires everything due since the last call, records whatever came in, and
    /// wraps at the loop point. Call it often -- the firing jitter is exactly
    /// this function's period.
    void poll();

    // ── ISequencerEngine ──────────────────────────────────────────────────

    void setLength(uint8_t beats) override;
    void setQuantization(Quantization q) override { quant_ = q; }
    void setLoop(bool loop) override { loop_ = loop; }
    void setPreLen(uint8_t len) override { preLen_ = len; }

    void selectSeqRecord(uint8_t seq) override { if (seq < kNumSequences) recSeq_ = seq; }
    void selectDirect(uint8_t seq, bool enabled) override {
        if (seq < kNumSequences) seqEnabled_[seq] = enabled;
    }
    void select(uint8_t seq) override { if (seq < kNumSequences) plySeq_ = seq; }

    void playback() override;
    void record() override;
    void overdub() override;
    void stop() override;
    void reset() override;

    void clearAll(uint8_t startPad, uint8_t endPad) override;
    void clearSeq(uint8_t seqIdx, uint8_t endPad) override;

    void setClickAudible(bool audible) override { if (clock_) clock_->setAudible(audible); }
    void setClickBpm(float bpm) override { if (clock_) clock_->setBpm(bpm); }
    void resetClick() override { if (clock_) clock_->resetPosition(); }
    float getClickBpm() const override { return clock_ ? clock_->bpm() : 120.0f; }
    int getClickPos() const override { return clock_ ? (int)clock_->currentBeat() : 0; }

    // ── State, for the UI and HIL ─────────────────────────────────────────

    SequencerMode  mode()  const { return mode_; }
    SequencerState state() const { return state_; }
    uint8_t recordSequence() const { return recSeq_; }
    bool    sequenceEnabled(uint8_t s) const { return s < kNumSequences && seqEnabled_[s]; }
    uint16_t eventCount(uint8_t s) const { return s < kNumSequences ? counts_[s] : 0; }
    uint16_t eventCount() const;
    /// Bars the loop spans -- the metronome owns it, this is the readback.
    uint8_t  bars() const { return clock_ ? clock_->barCount() : 0; }
    /// 0..1 through the loop, for a playhead.
    float    progress() const;

    /// Bars directly, which is how the UI thinks about it.
    void setBars(uint8_t bars);

    /// One recorded event, for tests and any UI that draws the pattern.
    bool eventAt(uint8_t seq, uint16_t i, uint32_t& pos, uint8_t& pad, uint8_t& vel) const {
        if (seq >= kNumSequences || i >= counts_[seq]) return false;
        pos = events_[seq][i].pos;
        pad = events_[seq][i].pad;
        vel = events_[seq][i].vel;
        return true;
    }

private:
    /// `fresh` marks an event recorded during the pass now playing. Without it
    /// a hit quantised a few samples backwards lands inside the window about to
    /// fire and sounds twice -- once from the pad, once from its own recording.
    /// Cleared at the wrap, where the loop legitimately comes round to it.
    struct Event { uint32_t pos; uint8_t pad; uint8_t vel; uint8_t fresh; };
    struct Hit   { uint32_t pos; uint8_t pad; uint8_t vel; };

    /// Rounds to the nearest grid line, wrapping a hit that rounds up past the
    /// loop end back to zero -- a hit a hair early on beat one belongs at the
    /// start of the loop, not the end of it.
    uint32_t quantise(uint32_t pos, uint32_t loop) const;
    void     drainInput(uint32_t loop);
    void     fireWindow(uint32_t from, uint32_t to);
    void     addEvent(uint8_t seq, uint32_t pos, uint8_t pad, uint8_t vel);

    MetronomeNode* clock_   = nullptr;
    NoteSink       sink_    = nullptr;
    void*          sinkCtx_ = nullptr;

    Event    events_[kNumSequences][kMaxEvents] = {};
    uint16_t counts_[kNumSequences] = {};
    bool     seqEnabled_[kNumSequences] = { true, true, true, true };

    Hit                   ring_[kInputRing] = {};
    std::atomic<uint32_t> ringHead_{0};   // producer
    std::atomic<uint32_t> ringTail_{0};   // consumer

    SequencerMode  mode_  = SequencerMode::None;
    SequencerState state_ = SequencerState::Idle;
    Quantization   quant_ = SequencerLimits::DEFAULT_QUANTIZATION;
    uint8_t  recSeq_  = 0;
    uint8_t  plySeq_  = 0;
    uint8_t  preLen_  = 4;
    bool     loop_    = true;
    /// One place that moves the transport, so the controller can never
    /// disagree with the engine about what is running.
    void setTransport(SequencerMode m, SequencerState st);

    SequencerController* controller_ = nullptr;
    uint32_t lastPos_ = 0;
};

} // namespace crosspad

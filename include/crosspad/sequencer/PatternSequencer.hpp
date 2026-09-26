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
/// **Scenes, not layers.** One sequence sounds at a time and each carries its
/// own bar count; selecting another queues it and the swap happens at the loop
/// end, never mid-bar. That is what makes per-scene lengths simple: the active
/// scene's length *is* the loop, pushed into the metronome so the click's
/// downbeat and the pattern's bar one are the same instant. Layering scenes
/// with different lengths would be polymetric and is deliberately not what this
/// does.
///
/// Threading, which is the whole reason for the ring below: hits arrive on
/// whichever thread runs the pads and must be stamped *there*, at the moment
/// they happen, or the timestamp inherits the queue latency it was supposed to
/// measure. Playback runs on whatever thread calls poll(). So noteIn() only
/// pushes into a single-producer/single-consumer ring and poll() drains it --
/// no lock is taken on either side, and neither can block the other.
class PatternSequencer : public ISequencerEngine {
public:
    /// Fired for every note due, from the thread that calls poll(). `on` false
    /// is the release: a pad that is pressed and never released stays latched
    /// as held, which lights its LED for good and leaves every reader of
    /// "is this pad pressed" answering yes forever.
    using NoteSink = void (*)(void* ctx, uint8_t pad, uint8_t velocity, bool on);

    /// Scene slots. Not MAX_SEQ_INDEX + 1 any more: that constant is the
    /// controller's own bookkeeping for solo and sequence navigation, while
    /// this is how many scenes a kit may hold. How many it actually holds is
    /// sceneCount(), which a player grows and shrinks; the rest of the slots
    /// cost their .bss and nothing else.
    static constexpr uint8_t  kNumSequences = 8;
    static constexpr uint16_t kMaxEvents    = 192;   ///< per scene
    static constexpr uint8_t  kInputRing    = 32;    ///< hits in flight
    static constexpr uint8_t  kMaxBars      = 8;     ///< per scene
    static constexpr uint8_t  kDefaultBars  = 4;
    /// Ticks per quarter note, for storage only. A saved pattern must survive
    /// being loaded at another tempo or sample rate, and a position in samples
    /// does not -- so the file holds ticks and the conversion happens on the
    /// way in and out. 96 divides a sixteenth (24) and a triplet (32) exactly.
    static constexpr uint16_t kPpq = 96;

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
    /// What a take is being snapped to, or Off. For the UI and HIL.
    Quantization quantization() const { return quant_; }
    void setLoop(bool loop) override { loop_ = loop; }
    /// Count-in, in beats: the click plays them before a REC or DUB started
    /// from a stop, and the take begins on the beat after. 0 = none.
    void setPreLen(uint8_t len) override { preLen_ = len; }
    uint8_t preLen() const override { return preLen_; }
    uint8_t countInLeft() const override {
        return (state_ == SequencerState::PreBeat && clock_) ? clock_->countInBeatsLeft() : 0;
    }

    /// Recording always targets the scene that is sounding -- you record into
    /// what you hear -- so this only moves the selection.
    void selectSeqRecord(uint8_t seq) override { select(seq); }
    /// Whether a scene takes part in "next scene" cycling. An empty scene you
    /// are not using is worth stepping over rather than through.
    void selectDirect(uint8_t seq, bool enabled) override {
        if (seq < kNumSequences) sceneEnabled_[seq] = enabled;
    }
    /// Queue a scene. It becomes active at the end of the current loop, or at
    /// once when nothing is running.
    void select(uint8_t seq) override;
    void selectScene(uint8_t seq) override { select(seq); }

    void playback() override;
    /// Play from @p pos samples into the loop, stopping first if running:
    /// the scene follows another timeline (the audio song) instead of
    /// starting at its own bar one.
    void playbackAt(uint32_t pos);
    void record() override;
    void overdub() override;
    void stop() override;
    void reset() override;

    void clearAll(uint8_t startPad, uint8_t endPad) override;
    void clearSeq(uint8_t seqIdx, uint8_t endPad) override;

    void setClickAudible(bool audible) override { if (clock_) clock_->setAudible(audible); }
    /// The tempo everything is counted at, and the one a kit carries in
    /// kit.json. Not a plain forward to the clock: a beat becomes a different
    /// number of samples, so whatever is already recorded is rescaled with it.
    void setClickBpm(float bpm) override;
    void resetClick() override { if (clock_) clock_->resetPosition(); }
    float getClickBpm() const override { return clock_ ? clock_->bpm() : 120.0f; }
    int getClickPos() const override { return clock_ ? (int)clock_->currentBeat() : 0; }

    // ── State, for the UI and HIL ─────────────────────────────────────────

    SequencerMode  mode()  const { return mode_; }
    SequencerState state() const { return state_; }
    uint8_t  sceneCount() const override { return sceneCount_; }
    bool     addScene() override;
    bool     removeScene(uint8_t seq) override;
    /// For the store: how many scenes the file held.
    void     setSceneCount(uint8_t n);
    uint8_t  activeScene()  const override { return activeSeq_; }
    uint8_t  pendingScene() const override { return pendingSeq_; }
    uint8_t  recordSequence() const { return activeSeq_; }
    bool     sceneEnabled(uint8_t s) const { return s < kNumSequences && sceneEnabled_[s]; }
    uint16_t eventCount(uint8_t s) const { return s < kNumSequences ? counts_[s] : 0; }
    uint16_t sceneEventCount(uint8_t s) const override { return eventCount(s); }
    bool     sceneEvent(uint8_t seq, uint16_t i, float& at, uint8_t& pad, uint8_t& vel) const override;
    uint16_t eventCount() const;
    /// Bars of the scene that is sounding.
    uint8_t  bars() const { return sceneBars(activeSeq_); }
    uint8_t  sceneBars(uint8_t seq) const override { return seq < kNumSequences ? bars_[seq] : 0; }
    /// 0..1 through the loop, for a playhead.
    float    progress() const override;

    /// Bars of the scene that is sounding. Applied at once: the loop this sets
    /// is the loop the click counts.
    void setBars(uint8_t bars) { setSceneBars(activeSeq_, bars); }
    void setSceneBars(uint8_t seq, uint8_t bars) override;
    bool doubleScene(uint8_t seq) override;
    bool halveScene(uint8_t seq) override;
    /// Held erase. Atomic: set from the UI, read where hits are drained.
    void setErasing(bool on) override { erasing_.store(on, std::memory_order_relaxed); }
    bool erasing() const override { return erasing_.load(std::memory_order_relaxed); }

    // ── Song ──────────────────────────────────────────────────────────────
    // Blocks are edited from the UI thread and read at the wrap on the poll
    // thread. Each block is one atomic word and the length another, so a read
    // never sees half a block; an insert racing a wrap can at worst have that
    // one wrap pick the neighbouring block. Taking a lock at the wrap would put
    // the UI thread in the beat's way, which is the worse trade.
    static constexpr uint8_t kSongMax        = 32;
    static constexpr uint8_t kSongMaxRepeats = 16;

    uint8_t songLength() const override { return songLen_.load(std::memory_order_acquire); }
    bool    songBlock(uint8_t i, uint8_t& scene, uint8_t& repeats) const override;
    bool    songInsert(uint8_t i, uint8_t scene, uint8_t repeats) override;
    bool    songRemove(uint8_t i) override;
    bool    songSet(uint8_t i, uint8_t scene, uint8_t repeats) override;
    void    setSongMode(bool on) override { songMode_.store(on, std::memory_order_relaxed); }
    bool    songMode() const override { return songMode_.load(std::memory_order_relaxed); }
    void    setSongLoop(bool on) override { songLoop_.store(on); markDirty(); }
    bool    songLoop() const override { return songLoop_.load(); }
    uint8_t songPosition() const override { return songPos_.load(std::memory_order_relaxed); }
    uint8_t songPass() const override { return songPass_.load(std::memory_order_relaxed); }
    void    songLocate(uint8_t i) override;

    /// One recorded event, for storage, tests and any UI that draws the pattern.
    bool eventAt(uint8_t seq, uint16_t i, uint32_t& pos, uint8_t& pad, uint8_t& vel) const {
        if (seq >= kNumSequences || i >= counts_[seq]) return false;
        pos = events_[seq][i].pos;
        pad = events_[seq][i].pad;
        vel = events_[seq][i].vel;
        return true;
    }

    // ── Storage ───────────────────────────────────────────────────────────
    // Positions cross this boundary as ticks, never samples: see kPpq.

    /// Back to what a kit nobody has recorded into looks like: no events,
    /// default lengths, scene one active. Clearing only the events would leave
    /// the previous kit's bar counts behind, which is not "empty".
    void resetScenes();

    /// Add an event straight into a scene, position in ticks. For loading.
    /// Not RT-safe against a running poll(); load while stopped.
    bool addEventTicks(uint8_t seq, uint32_t ticks, uint8_t pad, uint8_t vel);
    /// One event with its position in ticks, for saving.
    bool eventAtTicks(uint8_t seq, uint16_t i, uint32_t& ticks, uint8_t& pad, uint8_t& vel) const;
    /// True when something changed that the card does not have yet. The store
    /// clears it once the write lands. Atomic because the flag is set on the
    /// poll thread and read on whichever thread owns the card.
    bool dirty() const { return dirty_.load(std::memory_order_relaxed); }
    void clearDirty() { dirty_.store(false, std::memory_order_relaxed); }
    void markDirty()  { dirty_.store(true,  std::memory_order_relaxed); }
    bool isRecording() const {
        return (mode_ == SequencerMode::Record || mode_ == SequencerMode::Overdub)
               && state_ == SequencerState::Running;
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
    /// Drop every event of @p pad from @p seq. Poll thread only.
    void     erasePad(uint8_t seq, uint8_t pad);
    /// At a loop end in song mode: count the pass, move to the next block
    /// when this one is done, and hand its scene the loop. False when the
    /// song ran out and stopped the transport.
    bool     songAdvance();
    static uint16_t packBlock(uint8_t scene, uint8_t repeats) {
        return (uint16_t)((scene << 8) | repeats);
    }
    /// Make a queued scene the one that sounds, and hand its length to the
    /// click. Only ever called at a loop end, or while stopped.
    /// @p remember false for a change the song made: the file records the
    /// scene a player left, not every block a song passed through.
    void     applyScene(uint8_t seq, bool remember = true);
    /// One place that moves the transport, so the controller can never
    /// disagree with the engine about what is running.
    void     setTransport(SequencerMode m, SequencerState st);
    /// Start the clock for a take: through a count-in when one is set and the
    /// clock was stopped, straight in otherwise.
    void     startTake(SequencerMode m);

    MetronomeNode* clock_   = nullptr;
    NoteSink       sink_    = nullptr;
    void*          sinkCtx_ = nullptr;

    Event    events_[kNumSequences][kMaxEvents] = {};
    uint16_t counts_[kNumSequences] = {};
    uint8_t  bars_[kNumSequences] = { kDefaultBars, kDefaultBars, kDefaultBars, kDefaultBars };
    bool     sceneEnabled_[kNumSequences] = { true, true, true, true };

    Hit                   ring_[kInputRing] = {};
    std::atomic<uint32_t> ringHead_{0};   // producer
    std::atomic<uint32_t> ringTail_{0};   // consumer

    SequencerMode  mode_  = SequencerMode::None;
    SequencerState state_ = SequencerState::Idle;
    Quantization   quant_ = SequencerLimits::DEFAULT_QUANTIZATION;
    /// A kit starts with one scene and gets more when somebody asks for them.
    uint8_t  sceneCount_ = 1;
    uint8_t  activeSeq_  = 0;
    uint8_t  pendingSeq_ = 0;
    std::atomic<uint8_t> preLen_{4};
    bool     loop_       = true;
    std::atomic<bool> dirty_{false};
    std::atomic<bool> erasing_{false};

    std::atomic<uint16_t> song_[kSongMax] = {};
    std::atomic<uint8_t>  songLen_{0};
    std::atomic<bool>     songMode_{false};
    std::atomic<bool>     songLoop_{true};
    std::atomic<uint8_t>  songPos_{0};
    std::atomic<uint8_t>  songPass_{0};
    /// A block asked for while running, taken at the next loop end; -1 none.
    std::atomic<int16_t>  songJump_{-1};

    SequencerController* controller_ = nullptr;
    uint32_t lastPos_ = 0;
    /// Pads fired on the previous poll, released at the start of the next one.
    /// One poll of gate -- shorter than any human tap, long enough that the
    /// press and the release are not the same instant.
    uint16_t pendingRelease_ = 0;
};

} // namespace crosspad

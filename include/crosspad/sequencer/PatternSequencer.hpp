// SPDX-License-Identifier: MIT
#pragma once

#include "crosspad/sequencer/ISequencerEngine.hpp"
#include "crosspad/sequencer/MetronomeNode.hpp"
#include "crosspad/sequencer/SequencerController.hpp"
#include "crosspad/sequencer/SequencerTypes.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>

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
///
/// Editing a scene (bars, double, halve, removing one, loading) and poll() are
/// another matter: both rewrite the event arrays, so they take editMutex_.
/// poll() runs on a task, never the audio thread, and an edit holds the lock
/// for one pass over at most kMaxEvents events -- microseconds a poll can wait.
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
    /// The pad let go: how long it was held becomes the length of the note
    /// its press recorded. Wait-free, like noteIn().
    void noteRelease(uint8_t pad);

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
    /// Any pre-length turns the count-in on: it is always one bar of the
    /// metre the click is in, whatever length the caller had in mind.
    void setPreLen(uint8_t len) override { setCountIn(len > 0); }
    void setCountIn(bool on) override { countIn_.store(on, std::memory_order_relaxed); }
    bool countIn() const override { return countIn_.load(std::memory_order_relaxed); }
    uint8_t countInBeats() const override {
        return countIn() && clock_ ? clock_->beatsPerBar() : 0;   // a bar of it
    }

    void setSwing(uint8_t percent) override {
        swing_.store(percent > kMaxSwing ? kMaxSwing : percent, std::memory_order_relaxed);
    }
    uint8_t swing() const override { return swing_.load(std::memory_order_relaxed); }
    void setSwingPerBeat(uint8_t perBeat) override {
        if (perBeat == 2 || perBeat == 4) swingPerBeat_.store(perBeat, std::memory_order_relaxed);
    }
    uint8_t swingPerBeat() const override { return swingPerBeat_.load(std::memory_order_relaxed); }
    /// Past this the off-beat would land on the next subdivision, which is not
    /// a feel any more -- it is the note moved.
    static constexpr uint8_t kMaxSwing = 75;
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
    /// starting at its own bar one. With @p countInBeats the clock counts
    /// them first and the scene waits in PreBeat, as a take does.
    void playbackAt(uint32_t pos, uint8_t countInBeats = 0);
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
    float    sceneEventLength(uint8_t seq, uint16_t i) const override;
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
    /// Twice as long by playing it twice: every event copied one old loop
    /// later. Refused while that scene is taking a recording, and when the
    /// bars or the events would not fit, rather than doubling half a pattern.
    bool doubleScene(uint8_t seq) override;
    /// Half as long, keeping the first half; false below two bars.
    bool halveScene(uint8_t seq) override;
    /// Held erase: set from the UI, read where hits are drained.
    void setErasing(bool on) override { erasing_.store(on, std::memory_order_relaxed); }
    bool erasing() const override { return erasing_.load(std::memory_order_relaxed); }

    // ── Song ──────────────────────────────────────────────────────────────
    // Edited under editMutex_ like the scenes; each block is one atomic word
    // and the length another, so the UI reads them without the lock.
    static constexpr uint8_t kSongMax        = 32;
    static constexpr uint8_t kSongMaxRepeats = 16;

    uint8_t songLength() const override { return songLen_.load(std::memory_order_acquire); }
    bool    songBlock(uint8_t i, uint8_t& scene, uint8_t& repeats) const override;
    bool    songInsert(uint8_t i, uint8_t scene, uint8_t repeats) override;
    bool    songRemove(uint8_t i) override;
    bool    songSet(uint8_t i, uint8_t scene, uint8_t repeats) override;
    void    setSongMode(bool on) override;
    bool    songMode() const override { return songMode_.load(std::memory_order_relaxed); }
    void    setSongLoop(bool on) override { songLoop_.store(on); markDirty(); }
    bool    songLoop() const override { return songLoop_.load(); }
    uint8_t songPosition() const override { return songPos_.load(std::memory_order_relaxed); }
    uint8_t songPass() const override { return songPass_.load(std::memory_order_relaxed); }
    void    songLocate(uint8_t i) override;

    /// One recorded event, for storage, tests and any UI that draws the pattern.
    bool eventAt(uint8_t seq, uint16_t i, uint32_t& pos, uint8_t& pad, uint8_t& vel) const {
        std::lock_guard<std::recursive_mutex> lock(editMutex_);
        if (seq >= kNumSequences || i >= counts_[seq]) return false;
        pos = events_[seq][i].pos;
        pad = events_[seq][i].pad;
        vel = events_[seq][i].vel;
        return true;
    }
    /// How long event @p i is held, in samples; 0 for a tap -- one made before
    /// lengths were recorded, or one whose pad is still down.
    uint32_t eventLength(uint8_t seq, uint16_t i) const {
        std::lock_guard<std::recursive_mutex> lock(editMutex_);
        if (seq >= kNumSequences || i >= counts_[seq] || events_[seq][i].open) return 0;
        return events_[seq][i].len;
    }

    // ── Storage ───────────────────────────────────────────────────────────
    // Positions cross this boundary as ticks, never samples: see kPpq.

    /// Back to what a kit nobody has recorded into looks like: no events,
    /// default lengths, scene one active. Clearing only the events would leave
    /// the previous kit's bar counts behind, which is not "empty".
    void resetScenes();

    /// Add an event straight into a scene, position in ticks. For loading.
    /// Not RT-safe against a running poll(); load while stopped.
    /// @p lenTicks 0 is a tap: released on the poll after it fires.
    bool addEventTicks(uint8_t seq, uint32_t ticks, uint8_t pad, uint8_t vel, uint32_t lenTicks = 0);
    /// One event with its position in ticks, for saving.
    bool eventAtTicks(uint8_t seq, uint16_t i, uint32_t& ticks, uint8_t& pad, uint8_t& vel) const;
    /// eventLength() in ticks, for saving.
    uint32_t eventLengthTicks(uint8_t seq, uint16_t i) const;
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
    /// `len` is how long the pad was held, 0 for a tap. `open` marks a note
    /// whose pad is still down in a take: its length is not known yet.
    struct Event { uint32_t pos; uint32_t len; uint8_t pad; uint8_t vel; uint8_t fresh; uint8_t open; };
    /// `early`: count-in samples still to play when the hit was made, 0 once
    /// the loop runs. The clock holds position 0 through a count-in, so this
    /// is the only record of how far before beat one a hit was. `elapsed` is
    /// the clock's unwrapped count, what a note's length is measured on. `on`
    /// false is a release.
    struct Hit   { uint32_t pos; uint32_t early; uint32_t elapsed; uint8_t pad; uint8_t vel; uint8_t on; };

    /// Rounds to the nearest grid line, wrapping a hit that rounds up past the
    /// loop end back to zero -- a hit a hair early on beat one belongs at the
    /// start of the loop, not the end of it.
    uint32_t quantise(uint32_t pos, uint32_t loop) const;
    /// Samples of the subdivision swing is applied to, 0 when swing is off.
    uint32_t swingSamples() const;
    /// Where @p pos sounds with the swing on: every second subdivision later by
    /// the swing's share of one, wrapping at the loop end.
    uint32_t swung(uint32_t pos) const;
    /// The inverse, for a hit recorded while swing was on: the player aimed at
    /// what they heard, so the heard position comes back to the straight grid
    /// before it is quantised. Without this, recording a swung eighth lands it
    /// on the next sixteenth.
    uint32_t unswung(uint32_t pos) const;
    /// Grid lines to the beat: 1, 2, 3, 4, 6 or 8, and 0 with quantisation
    /// off. The triplets are what make it a count rather than a divisor.
    uint8_t  gridPerBeat() const;
    /// The grid a take snaps to in samples, 0 with quantisation off. Rounded
    /// down, so it answers "about how wide is a step" -- where a line actually
    /// falls is quantise()'s business.
    uint32_t gridSamples() const;
    /// How far before beat one a count-in hit may be and still land on it:
    /// half a grid step, which is where it would have rounded anyway.
    uint32_t downbeatGrace() const;
    void     drainInput(uint32_t loop);
    /// Push a hit into the ring; dropped when the ring is full.
    void     pushHit(uint8_t pad, uint8_t vel, bool on);
    /// Give the open note of @p pad its length: from its press to @p elapsed,
    /// on the clock's unwrapped count, at most one loop.
    void     closeNote(uint8_t pad, uint32_t elapsed, uint32_t loop);
    /// Close every open note at @p elapsed -- a take that ends with pads down.
    void     closeAllNotes(uint32_t elapsed, uint32_t loop);
    void     fireWindow(uint32_t from, uint32_t to);
    /// Release every note whose length runs out inside [from, to).
    void     releaseWindow(uint32_t from, uint32_t to);
    /// Hand every pad held by a length to the next poll's release: the loop
    /// it was timed against has changed under it.
    void     dropGates();
    /// poll() under editMutex_; its presses are collected, not played.
    void     pollLocked();
    /// A press or release for the sink: played at once, or -- inside poll()
    /// -- collected and played once the lock is let go. The sink runs pad
    /// logic that can wait on the sample player; holding the lock through it
    /// held every UI read of the pattern for as long.
    void     emit(uint8_t pad, uint8_t vel, bool on);
    /// Returns the index the event went in, or -1 when the scene is full.
    int      addEvent(uint8_t seq, uint32_t pos, uint8_t pad, uint8_t vel);
    /// Drop every event of @p pad from @p seq.
    void     erasePad(uint8_t seq, uint8_t pad);
    /// setSceneBars() without taking the lock, for callers that hold it.
    void     applySceneBars(uint8_t seq, uint8_t bars);
    /// At a loop end in song mode: count the pass, move to the next block
    /// when this one is done, and hand its scene the loop. False when the
    /// song ran out and stopped the transport.
    bool     songAdvance();
    static uint16_t packBlock(uint8_t scene, uint8_t repeats) {
        return (uint16_t)((scene << 8) | repeats);
    }
    /// Make a queued scene the one that sounds, and hand its length to the
    /// click. Only ever called at a loop end, or while stopped. @p remember
    /// false for a change the song made: the file records the scene a player
    /// left, not every block a song passed through.
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
    std::atomic<bool> countIn_{false};
    std::atomic<uint8_t> swing_{0};          ///< percent of a subdivision, 0 = straight
    std::atomic<uint8_t> swingPerBeat_{2};   ///< 2 = eighths, 4 = sixteenths
    bool     loop_       = true;
    std::atomic<bool> dirty_{false};
    std::atomic<bool> erasing_{false};
    mutable std::recursive_mutex editMutex_;
    struct Press { uint8_t pad; uint8_t vel; bool on; };
    /// A poll fires at most a whole scene, releases each of those notes, and
    /// every pad held from before it.
    static constexpr uint16_t kMaxPresses = 2 * kMaxEvents + 32;
    /// poll()'s collection, here rather than on the poll task's stack: it is
    /// over a kilobyte, and only the one thread that polls ever fills it.
    Press    pressBuf_[kMaxPresses] = {};
    Press*   presses_    = nullptr;   // set only while poll() collects
    uint16_t pressCount_ = 0;

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
    uint32_t preBeatPos_ = 0;   // where the loop starts once a count-in hands over
    /// Pads fired on the previous poll, released at the start of the next one.
    /// One poll of gate -- shorter than any human tap, long enough that the
    /// press and the release are not the same instant.
    uint16_t pendingRelease_ = 0;
    /// Pads held by a recorded length, each released when the loop reaches
    /// its releaseAt_.
    uint16_t gated_ = 0;
    uint32_t releaseAt_[16] = {};
    /// When each pad's open note was pressed, on the clock's unwrapped count.
    uint32_t openAt_[16] = {};
};

} // namespace crosspad

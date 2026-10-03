#pragma once
#include <cstdint>
#include <crosspad/sequencer/SequencerTypes.hpp>

namespace crosspad {

/**
 * @brief Abstract interface for the underlying MIDI sequencer + click engine.
 *
 * Platform implementations wrap concrete engines (e.g. ML_MidiSeq + ml_click
 * on ESP32, or a software implementation for desktop/tests).
 */
class ISequencerEngine {
public:
    virtual ~ISequencerEngine() = default;

    // --- Sequence parameters ---
    virtual void setLength(uint8_t beats) = 0;
    virtual void setQuantization(Quantization q) = 0;
    virtual void setLoop(bool loop) = 0;
    virtual void setPreLen(uint8_t len) = 0;
    /// A bar of click before a take started from a stop. An engine without
    /// one ignores it and reports it off.
    virtual void    setCountIn(bool on) { (void)on; }
    virtual bool    countIn() const { return false; }
    /// How long the count-in is, in beats, and 0 when it is off -- the one
    /// answer to "how much PRE", so a take started from somewhere else (the
    /// song's transport, with the sequencer in sync) counts the same beats as
    /// a take started here.
    virtual uint8_t countInBeats() const { return 0; }
    /// Beats of count-in still to go, 0 when not counting in.
    virtual uint8_t countInLeft() const { return 0; }

    // --- Swing ---
    /// How late every second subdivision sounds, as a percentage of one. 0 is
    /// straight; 50 % puts the off-beat two thirds of the way through the pair,
    /// which is the triplet feel. An engine without swing ignores it and
    /// reports it off.
    ///
    /// Playback only: the pattern keeps its straight positions and the feel is
    /// applied on the way out, so turning swing down leaves what was recorded
    /// exactly where it was. A take made while it is on is pulled back to the
    /// straight grid first -- the player aims at what they hear.
    virtual void    setSwing(uint8_t percent) { (void)percent; }
    virtual uint8_t swing() const { return 0; }
    /// Which subdivision is swung, as a count to the beat: 2 for eighths, 4 for
    /// sixteenths. Anything else is ignored.
    virtual void    setSwingPerBeat(uint8_t perBeat) { (void)perBeat; }
    virtual uint8_t swingPerBeat() const { return 2; }

    // --- Sequence selection ---
    virtual void selectSeqRecord(uint8_t seq) = 0;
    virtual void selectDirect(uint8_t seq, bool enabled) = 0;
    virtual void select(uint8_t seq) = 0;

    // --- Transport ---
    virtual void playback() = 0;
    virtual void record() = 0;
    virtual void overdub() = 0;
    virtual void stop() = 0;
    virtual void reset() = 0;

    // --- Erase ---
    virtual void clearAll(uint8_t startNote, uint8_t endNote) = 0;
    virtual void clearSeq(uint8_t seqIdx, uint8_t endNote) = 0;

    // --- Scenes (optional) ---
    // One sequence sounds at a time and each carries its own length. An engine
    // that has no such notion keeps these defaults and behaves as it always
    // did -- they are virtual with bodies, not pure, so adding them here does
    // not break an implementation written before they existed.
    /// Queue a scene; an engine that loops will swap at the loop end.
    virtual void     selectScene(uint8_t seq) { select(seq); }
    virtual uint8_t  activeScene() const { return 0; }
    /// The scene waiting for the loop end, or activeScene() when none is.
    virtual uint8_t  pendingScene() const { return activeScene(); }
    virtual void     setSceneBars(uint8_t seq, uint8_t bars) { (void)seq; (void)bars; }
    virtual uint8_t  sceneBars(uint8_t seq) const { (void)seq; return 0; }
    /// How many scenes the kit has. An engine with a fixed set says so here.
    virtual uint8_t  sceneCount() const { return 1; }
    /// Append an empty scene and make it current; false when there is no room.
    virtual bool     addScene() { return false; }
    /// Drop a scene and close the gap. The last one never goes.
    virtual bool     removeScene(uint8_t seq) { (void)seq; return false; }
    virtual uint16_t sceneEventCount(uint8_t seq) const { (void)seq; return 0; }
    // Editing and drawing a scene. An engine that cannot answers false / 0
    // (the Arduino ML_MidiSeq one), and a UI hides what it gets false for.
    /// Event @p i of a scene for drawing: where it sits as 0..1 of the scene's
    /// loop, which pad, how hard.
    virtual bool     sceneEvent(uint8_t seq, uint16_t i, float& at, uint8_t& pad, uint8_t& vel) const {
        (void)seq; (void)i; (void)at; (void)pad; (void)vel; return false;
    }
    /// Twice the bars, the second half a copy of the first; false when it
    /// would not fit.
    virtual bool     doubleScene(uint8_t seq) { (void)seq; return false; }
    /// Half the bars, keeping the first half; false at one bar.
    virtual bool     halveScene(uint8_t seq) { (void)seq; return false; }
    /// While on, a pad hit erases that pad from the sounding scene instead of
    /// playing into it.
    virtual void     setErasing(bool on) { (void)on; }
    virtual bool     erasing() const { return false; }
    /// 0..1 through the loop that is sounding, for a playhead.
    virtual float    progress() const { return 0.0f; }

    // --- Song (optional; an engine without one reports an empty song) ---
    // An arrangement: blocks of {scene, repeats} played in order, each scene
    // looped as many times as its block says before the next one takes over
    // at the loop end. Only while song mode is on; stop() turns it off, so a
    // plain Play plays the scene as it always did.
    virtual uint8_t  songLength() const { return 0; }
    virtual bool     songBlock(uint8_t i, uint8_t& scene, uint8_t& repeats) const {
        (void)i; (void)scene; (void)repeats; return false;
    }
    /// Insert before @p i (i == songLength() appends).
    virtual bool     songInsert(uint8_t i, uint8_t scene, uint8_t repeats) {
        (void)i; (void)scene; (void)repeats; return false;
    }
    virtual bool     songRemove(uint8_t i) { (void)i; return false; }
    virtual bool     songSet(uint8_t i, uint8_t scene, uint8_t repeats) {
        (void)i; (void)scene; (void)repeats; return false;
    }
    /// Turning it on while stopped goes to the first block.
    virtual void     setSongMode(bool on) { (void)on; }
    virtual bool     songMode() const { return false; }
    virtual void     setSongLoop(bool on) { (void)on; }
    virtual bool     songLoop() const { return true; }
    /// Where the song is: the block and the pass through it (0-based).
    virtual uint8_t  songPosition() const { return 0; }
    virtual uint8_t  songPass() const { return 0; }
    /// Start from block @p i: at once when stopped, at the loop end when not.
    virtual void     songLocate(uint8_t i) { (void)i; }

    // --- Click / Metronome ---
    virtual void setClickAudible(bool audible) = 0;
    virtual void setClickBpm(float bpm) = 0;
    virtual void resetClick() = 0;
    virtual float getClickBpm() const = 0;
    /// Index of the current beat in the bar.
    virtual int getClickPos() const = 0;
};

} // namespace crosspad

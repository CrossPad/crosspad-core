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

    // --- Click / Metronome ---
    virtual void setClickAudible(bool audible) = 0;
    virtual void setClickBpm(float bpm) = 0;
    virtual void resetClick() = 0;
    virtual float getClickBpm() const = 0;
    /// Index of the current beat in the bar.
    virtual int getClickPos() const = 0;
};

} // namespace crosspad

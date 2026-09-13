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

    // --- Click / Metronome ---
    virtual void setClickAudible(bool audible) = 0;
    virtual void setClickBpm(float bpm) = 0;
    virtual void resetClick() = 0;
    virtual float getClickBpm() const = 0;
    /// Index of the current beat in the bar.
    virtual int getClickPos() const = 0;
};

} // namespace crosspad

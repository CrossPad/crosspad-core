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

    // --- Click / Metronome ---
    virtual void setClickAudible(bool audible) = 0;
    virtual void setClickBpm(float bpm) = 0;
    virtual void resetClick() = 0;
};

} // namespace crosspad

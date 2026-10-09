// SPDX-License-Identifier: MIT
#pragma once

#include "crosspad/instrument/InstrumentPlayState.hpp"
#include "crosspad/pad/IPadLogicHandler.hpp"

#include <atomic>
#include <cstdint>

namespace crosspad {

/**
 * @brief The pads playing the instrument Pitch Play is set to.
 *
 * Anything that offers "the instrument" as a source -- the Sequencer
 * recording one, the Recorder bouncing one -- needs the same sixteen pads to
 * make the same notes as pressing them in Pitch Play does. That is one
 * behaviour, so it lives once, here, rather than being written out again in
 * every app that wants it.
 *
 * Reads InstrumentPlayState for the map and PlatformServices::pitchedInstrument
 * for the engine, so there is nothing to configure: register it, push it while
 * the source is the instrument, pop it after.
 *
 * The notes a pad struck are kept until it is released. A player may change
 * the key, the octave or the chord with a pad still down, and a release that
 * recomputed the notes would stop ones that were never started and leave the
 * struck ones ringing.
 *
 * ready() gates it: a bank takes a moment to build, and a hit before it is
 * installed must be silent rather than a note into an engine with nothing in
 * it.
 */
class InstrumentPadLogic : public IPadLogicHandler {
public:
    static constexpr uint8_t kPads  = 16;
    static constexpr uint8_t kChord = 6;    ///< PlaySettings::padChordNotes' width

    /// The owner sets this true once the host reports the bank is playing.
    void setReady(bool on) { ready_.store(on, std::memory_order_release); }
    bool ready() const { return ready_.load(std::memory_order_acquire); }

    void onActivate(PadManager&) override {}
    void onDeactivate(PadManager&) override;
    void onPadPress(PadManager& pm, uint8_t pad, uint8_t velocity) override;
    void onPadRelease(PadManager& pm, uint8_t pad) override;
    void onPadPressure(PadManager&, uint8_t, uint8_t) override {}

private:
    std::atomic<bool>    ready_{false};
    std::atomic<uint8_t> note_[kPads][kChord] = {};
    std::atomic<uint8_t> held_[kPads] = {};
};

} // namespace crosspad

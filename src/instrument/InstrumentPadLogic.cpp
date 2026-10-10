// SPDX-License-Identifier: MIT
#include "crosspad/instrument/InstrumentPadLogic.hpp"

#include "crosspad/instrument/PitchedInstrument.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/platform/PlatformServices.hpp"

namespace crosspad {
namespace {

PitchedInstrument* engine() { return getPlatformServices().pitchedInstrument; }

} // namespace

void InstrumentPadLogic::onDeactivate(PadManager&)
{
    if (auto* p = engine()) p->allNotesOff();
}

void InstrumentPadLogic::onPadPress(PadManager& pm, uint8_t pad, uint8_t velocity)
{
    if (pad >= kPads || !ready()) return;
    auto* p = engine();
    if (!p) return;

    const PlaySettings& ps = instrumentPlayState().play;
    uint8_t notes[kChord] = {};
    uint8_t n = 0;
    const uint8_t have = ps.padChordCount[pad];
    for (uint8_t c = 0; c < have && c < kChord; ++c) {
        if (ps.padChordNotes[pad][c] != 0xFF) notes[n++] = ps.padChordNotes[pad][c];
    }
    if (n == 0) notes[n++] = ps.padMidiMap[pad];   // no chord: the pad's own note

    const uint8_t vel = velocity ? velocity : 1;
    held_[pad].store(n, std::memory_order_relaxed);
    for (uint8_t i = 0; i < n; ++i) {
        note_[pad][i].store(notes[i], std::memory_order_relaxed);
        p->noteOn(notes[i], vel, pad);
    }
    pm.notifyNoteOn(pad, vel, true);
}

void InstrumentPadLogic::onPadRelease(PadManager& pm, uint8_t pad)
{
    if (pad >= kPads) return;
    auto* p = engine();
    const uint8_t n = held_[pad].exchange(0, std::memory_order_relaxed);
    for (uint8_t i = 0; i < n; ++i) {
        if (p) p->noteOff(note_[pad][i].load(std::memory_order_relaxed));
    }
    pm.notifyNoteOff(pad, true);
}

} // namespace crosspad

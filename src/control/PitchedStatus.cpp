// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/control/PitchedStatus.hpp"

#include "crosspad/instrument/PitchedInstrument.hpp"

namespace crosspad::control {

void readPitchedEngine(const PitchedInstrument& inst, PitchedStatus& out) {
    out.steals         = inst.stealCount();
    out.sheds          = inst.shedCount();
    out.activeVoices   = inst.activeVoices();
    out.soundingVoices = inst.soundingVoices();
    out.maxVoices      = inst.maxVoices();
    out.voiceCap       = inst.voiceCap();
    out.lastNoteHz     = inst.lastNoteHz();
}

void readPitchedBank(const SampleBank* bank, PitchedStatus& out) {
    out.zones     = bank ? bank->zoneCount() : 0;
    out.bankBytes = bank ? bank->bytes() : 0;
    for (uint8_t i = 0; i < SampleBank::kMaxZones; ++i) {
        out.roots[i] = i < out.zones ? bank->zone(i).rootMidi : 0.0f;
    }
}

} // namespace crosspad::control

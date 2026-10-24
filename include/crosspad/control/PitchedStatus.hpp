// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file PitchedStatus.hpp
 * @brief The pitched engine verb (board PITCHED_STATUS, simulator
 *        pitched_status), gathered once.
 *
 * Two halves, because they come from two owners: the engine's counters are
 * the PitchedInstrument's, the bank's shape is what the platform's kit port
 * installed. Whether a pitched kit is loaded at all (`active`) is the port's
 * to say; neither reader sets it.
 */

#include <cstddef>
#include <cstdint>

#include "crosspad/instrument/SampleBank.hpp"

namespace crosspad {
class PitchedInstrument;
}

namespace crosspad::control {

struct PitchedStatus {
    bool     active         = false;
    uint8_t  zones          = 0;
    float    roots[SampleBank::kMaxZones] = {};   ///< MIDI root of each zone, [0, zones)
    size_t   bankBytes      = 0;
    uint32_t steals         = 0;
    uint32_t sheds          = 0;   ///< steals an overloaded render forced below the cap
    uint8_t  activeVoices   = 0;
    uint8_t  soundingVoices = 0;
    uint8_t  maxVoices      = 0;   ///< in force now
    uint8_t  voiceCap       = 0;   ///< the platform's ceiling, from the next setParams
    float    lastNoteHz     = 0.0f;
};

/// The engine's counters: steals, sheds, voices, cap, last note.
void readPitchedEngine(const PitchedInstrument& inst, PitchedStatus& out);

/// Zones, their roots and the PCM size of @p bank; all zero for null.
void readPitchedBank(const SampleBank* bank, PitchedStatus& out);

} // namespace crosspad::control

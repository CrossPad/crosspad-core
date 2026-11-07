// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file BleMidiOffsets.hpp
 * @brief The transpose picklist a BLE MIDI output offers.
 *
 * BLE is the one output that regularly talks to something whose note map you
 * do not control — a phone, a DAW, another player's device — so it gets its
 * own transpose. The values are the ones drum-pad hosts actually ask for
 * rather than musical intervals: whole octaves, and the drum-rack bases that
 * Ableton, FL Studio and hardware pad samplers sit on. +36 is the common one — a
 * grid whose pad 1 sends note 0 lands on C1 (36), where Live's Drum Rack and
 * FPC expect it.
 *
 * Index 0 is Default: send exactly the note the router handed over, which is
 * the note USB MIDI is sending at the same moment. Labels stay short because
 * the settings row shows them inside a dropdown on a 320 px screen.
 *
 * One table, used by the settings UI and by the transport, so a label can
 * never drift from the value it selects.
 */

#include <cstdint>

namespace crosspad {

/// Semitone offsets, index 0 = Default (identical to what USB MIDI sends).
inline constexpr int8_t kBleMidiTxOffsets[] = {
    0, -48, -36, -24, -12, 12, 24, 36, 48, 60
};

/// Labels for the same indices, in the order the dropdown shows them.
inline constexpr const char* kBleMidiTxOffsetNames[] = {
    "Default",
    "-48 (C-2)",
    "-36 (C-1)",
    "-24",
    "-12",
    "+12",
    "+24",
    "+36 (C1)",
    "+48 (C2)",
    "+60 (C3)",
};

inline constexpr uint8_t kBleMidiTxOffsetCount =
    (uint8_t)(sizeof(kBleMidiTxOffsets) / sizeof(kBleMidiTxOffsets[0]));

static_assert(sizeof(kBleMidiTxOffsetNames) / sizeof(kBleMidiTxOffsetNames[0])
                  == kBleMidiTxOffsetCount,
              "every BLE MIDI transpose needs exactly one label");

/// Semitones for a picklist index; anything out of range means Default.
inline int8_t bleMidiTxOffsetValue(uint8_t index) {
    return index < kBleMidiTxOffsetCount ? kBleMidiTxOffsets[index] : 0;
}

/// Label for a picklist index; never null.
inline const char* bleMidiTxOffsetName(uint8_t index) {
    return index < kBleMidiTxOffsetCount ? kBleMidiTxOffsetNames[index]
                                         : kBleMidiTxOffsetNames[0];
}

} // namespace crosspad

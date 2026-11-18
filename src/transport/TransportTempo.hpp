// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace crosspad {

/* The tempo the board's own files keep. A host's MIDI clock puts its tempo on
 * the sequencer's clock every tick, and the board plays on at it once the host
 * lets go; neither makes it the groove's tempo. Until a tempo is set on the
 * board (transportSetTempo(), a file loaded with the host gone) or the clock
 * moves away from what the host left on it, a save keeps the tempo that was in
 * force when the host took over. */

/// What a file keeps for a clock running at @p clockBpm.
float transportTempoToSave(float clockBpm);
/// A file has put @p bpm on the clock: with a host driving it is the tempo a
/// save keeps from now on; with none, the clock's tempo is the board's again.
void  transportTempoLoaded(float bpm);

} // namespace crosspad

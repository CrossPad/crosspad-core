// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/music/MusicTheory.hpp"

#include <cstdint>
#include <string>

namespace crosspad {

/**
 * @brief What Pitch Play is set to, where the rest of the board can read it.
 *
 * The key, the scale, the transpose, the chord and the instrument are the
 * player's one setting -- not each app's own copy. A pattern recorded in the
 * Sequencer and a take bounced in the Recorder have to sound like pressing
 * the pads in Pitch Play does, and the only way that holds is for them to
 * read the same state instead of keeping their own.
 *
 * So: **Pitch Play writes, everyone else reads.** An app that wants the
 * instrument asks IInstrumentHost to open @c instrumentPath and maps its pads
 * from @c play, whose padMidiMap and padChordNotes MusicTheory::rebuildPadMap()
 * has already filled in. @c generation rises on every change, which is how a
 * reader notices without polling the whole structure.
 *
 * It is deliberately not an interface: there is nothing to implement and
 * nothing platform-specific about it. The pad map is arithmetic.
 */
struct InstrumentPlayState {
    /// rebuildPadMap() has run on this already; read padMidiMap /
    /// padChordNotes / padChordCount straight out of it.
    PlaySettings play;
    /// Semitones on top of @c play, which it is already in (-12..+12), and
    /// whether the pads play Pitch Play's chord list: the two settings
    /// PlaySettings has no field for, which a song keeps with the map
    /// (AudioSong::PitchPads) and Pitch Play takes back from here.
    int8_t transpose = 0;
    bool   chordsOn = false;

    /// Logical path of the instrument file, the way IInstrumentHost::requestOpen
    /// wants it ("/crosspad/pitchplay/<name>/instrument.json"). Empty means
    /// Pitch Play has none loaded, and then there is nothing to borrow.
    std::string instrumentPath;

    /// For a reader that shows the name without re-reading the file.
    std::string instrumentName;

    /// What a pad makes, named: "one note", a chord type, or a progression.
    /// Carried as text on purpose -- a reader cannot name it for itself,
    /// because the player's own progressions live in Pitch Play's
    /// MusicTheory instance and nowhere else, so a second instance would
    /// name a custom progression wrongly rather than not at all.
    std::string chordName;

    /// Rises on every change Pitch Play publishes. A reader keeps the last
    /// value it acted on and compares.
    uint32_t generation = 0;
};

/// The one instance. Pitch Play writes it; anything else reads.
InstrumentPlayState& instrumentPlayState();

/// Pitch Play calls this once it has finished changing the state, so a reader
/// sees a whole change rather than half of one.
void instrumentPlayStatePublished();

/// An instrument another screen chose for Pitch Play to open -- the kit
/// browser's PITCH list. Set just before Pitch Play is opened; it takes the
/// path as it starts, in place of the one it played last. Logical, as
/// IInstrumentHost::requestOpen() wants it. UI thread.
void instrumentOpenRequest(const std::string& path);

/// Takes it: true once per instrumentOpenRequest().
bool instrumentTakeOpenRequest(std::string& path);

} // namespace crosspad

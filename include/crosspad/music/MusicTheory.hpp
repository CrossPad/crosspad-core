// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <string.h>

namespace crosspad {

// ============================================================================
// Scale definitions (intervals from root)
// ============================================================================
struct ScaleDef {
    const char* name;
    uint8_t intervals[12];
    uint8_t count;
};

// ============================================================================
// Chord definitions (intervals from note)
// ============================================================================
struct ChordDef {
    const char* name;
    const char* shortName;
    uint8_t intervals[6];
    uint8_t count;
};

// ============================================================================
// Chord progressions
// ============================================================================
struct ProgStep {
    uint8_t degree;     // scale degree (0-based)
    uint8_t chordType;  // index into chords[] (0-11)
};

struct BuiltinProg {
    const char* name;
    ProgStep steps[8];
    uint8_t stepCount;
    /* The mode it is written in. A progression is degrees, so the same steps
     * are a different record in another scale -- "Backdoor" wants Mixolydian
     * for its flat seventh, "Lydian Dream" wants Lydian for its major II --
     * and the UI has somewhere to look rather than the player guessing. */
    uint8_t scale;
};

struct CustomProg {
    char name[16];
    ProgStep steps[16];
    uint8_t stepCount;
};

// ============================================================================
// Play settings — precomputed MIDI note mapping for 16 pads
// ============================================================================
struct PlaySettings {
    uint8_t rootNote;              // 0-11 (C=0, C#=1, ..., B=11)
    int8_t  octave;                // -2 to +4
    uint8_t scaleIndex;            // index into scale table
    uint8_t chordIndex;            // index into chord table
    uint8_t padMidiMap[16];        // precomputed MIDI note per pad
    uint8_t padChordNotes[16][6];  // up to 6 chord notes per pad (0xFF = unused)
    uint8_t padChordCount[16];     // how many chord notes per pad
    uint8_t padChordTypeIdx[16];   // which chord type index was used per pad
};

// ============================================================================
// MusicTheory — owns scale/chord data and pad-map computation
// ============================================================================
class MusicTheory {
public:
    static constexpr uint8_t MAX_CUSTOM_PROGS = 8;
    static constexpr uint8_t MAX_PROG_STEPS   = 16;

    // --- static scale/chord data access ---
    static uint8_t scaleCount();
    static uint8_t chordDefCount();       // raw chords[] size, modes included
    static uint8_t builtinProgCount();
    static const char* scaleName(uint8_t idx);
    static const char* noteName(uint8_t midiNote);
    static char chordQualChar(uint8_t chordTypeIdx);
    /// The scale a built-in progression is written in (an index into scales).
    static uint8_t builtinProgScale(uint8_t idx);

    /* Special chord indices. Everything below CHORD_DIATONIC_IDX is a chord a
     * pad can be built from; the two after it are modes, and anything from
     * CHORD_PROG_START_IDX up is a progression (the built-in ones first, then
     * the custom ones in the order they were added). The three moved up by
     * three when the ninths were added -- nothing persists them, so nothing
     * had to migrate. */
    static constexpr int CHORD_DIATONIC_IDX   = 20;
    static constexpr int CHORD_TYPES_IDX      = 21;
    static constexpr int CHORD_PROG_START_IDX = 22;

    /// How many of chords[] a pad can be built from: 0 .. CHORD_DIATONIC_IDX-1.
    static constexpr int playableChordCount() { return CHORD_DIATONIC_IDX; }

    // --- instance methods (custom progs state) ---
    bool addCustomProgression(const char* name, const ProgStep* steps, uint8_t stepCount);
    void clearCustomProgressions() { customProgCount_ = 0; }
    uint8_t customProgCount() const { return customProgCount_; }

    /// Total chord/mode count visible in UI (chords + builtin progs + custom progs)
    uint8_t totalChordCount() const;

    const char* chordName(uint8_t idx) const;
    const char* chordShortName(uint8_t idx) const;

    /// Recompute padMidiMap / padChordNotes for 16 pads based on settings
    void rebuildPadMap(PlaySettings& ps) const;

private:
    CustomProg customProgs_[MAX_CUSTOM_PROGS];
    uint8_t customProgCount_ = 0;
};

} // namespace crosspad

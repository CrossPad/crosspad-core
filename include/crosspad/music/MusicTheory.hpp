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
    static uint8_t chordDefCount();       // raw chords[] size (14)
    static uint8_t builtinProgCount();
    static const char* scaleName(uint8_t idx);
    static const char* noteName(uint8_t midiNote);
    static char chordQualChar(uint8_t chordTypeIdx);

    // Special chord indices
    static constexpr int CHORD_DIATONIC_IDX  = 12;
    static constexpr int CHORD_TYPES_IDX     = 13;
    static constexpr int CHORD_PROG_START_IDX = 14;

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

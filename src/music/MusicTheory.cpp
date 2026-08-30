#include <crosspad/music/MusicTheory.hpp>

namespace crosspad {


static const ScaleDef scales[] = {
    {"Chromatic",     {0,1,2,3,4,5,6,7,8,9,10,11}, 12},
    {"Major",         {0,2,4,5,7,9,11,0,0,0,0,0},   7},
    {"Minor",         {0,2,3,5,7,8,10,0,0,0,0,0},   7},
    {"Pentatonic",    {0,2,4,7,9,0,0,0,0,0,0,0},    5},
    {"Min Penta",     {0,3,5,7,10,0,0,0,0,0,0,0},   5},
    {"Blues",          {0,3,5,6,7,10,0,0,0,0,0,0},   6},
    {"Dorian",        {0,2,3,5,7,9,10,0,0,0,0,0},   7},
    {"Mixolydian",    {0,2,4,5,7,9,10,0,0,0,0,0},   7},
    {"Harmonic Min",  {0,2,3,5,7,8,11,0,0,0,0,0},   7},
};
static const uint8_t SCALE_COUNT = sizeof(scales) / sizeof(scales[0]);

// Indices 0-11 = normal chords, 12 = Diatonic, 13 = Types
static const ChordDef chords[] = {
    {"None",      "Root", {0,0,0,0,0,0},    1},
    {"Major",     "Maj",  {0,4,7,0,0,0},    3},
    {"Minor",     "Min",  {0,3,7,0,0,0},    3},
    {"7th",       "7th",  {0,4,7,10,0,0},   4},
    {"Maj7",      "M7",   {0,4,7,11,0,0},   4},
    {"Min7",      "m7",   {0,3,7,10,0,0},   4},
    {"Sus2",      "Su2",  {0,2,7,0,0,0},    3},
    {"Sus4",      "Su4",  {0,5,7,0,0,0},    3},
    {"Dim",       "Dim",  {0,3,6,0,0,0},    3},
    {"Aug",       "Aug",  {0,4,8,0,0,0},    3},
    {"Power",     "P5",   {0,7,0,0,0,0},    2},
    {"Octave",    "Oct",  {0,12,0,0,0,0},   2},
    {"Diatonic",  "Dia",  {0,0,0,0,0,0},    1},  // special: per-degree chord
    {"Types",     "Typ",  {0,0,0,0,0,0},    1},  // special: each pad = different type
};
static const uint8_t CHORD_COUNT = sizeof(chords) / sizeof(chords[0]);

// Diatonic chord qualities per scale degree
// Values are indices into chords[]: 1=Major, 2=Minor, 8=Dim, 9=Aug
static const uint8_t diatonicQuality[][12] = {
    // 0: Chromatic (12 notes)
    {1, 2, 1, 2, 2, 1, 2, 1, 2, 1, 2, 8},
    // 1: Major: I=Maj ii=min iii=min IV=Maj V=Maj vi=min vii=dim
    {1, 2, 2, 1, 1, 2, 8, 0, 0, 0, 0, 0},
    // 2: Minor: i=min ii=dim III=Maj iv=min v=min VI=Maj VII=Maj
    {2, 8, 1, 2, 2, 1, 1, 0, 0, 0, 0, 0},
    // 3: Pentatonic: I=Maj ii=min III=Maj V=Maj vi=min
    {1, 2, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0},
    // 4: Min Penta: i=min III=Maj iv=min v=min VII=Maj
    {2, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0},
    // 5: Blues: i=min III=Maj iv=min #iv=dim V=Maj bVII=min
    {2, 1, 2, 8, 1, 2, 0, 0, 0, 0, 0, 0},
    // 6: Dorian: i=min ii=min III=Maj IV=Maj v=min vi=dim VII=Maj
    {2, 2, 1, 1, 2, 8, 1, 0, 0, 0, 0, 0},
    // 7: Mixolydian: I=Maj ii=min iii=dim IV=Maj v=min vi=min VII=Maj
    {1, 2, 8, 1, 2, 2, 1, 0, 0, 0, 0, 0},
    // 8: Harmonic Min: i=min ii=dim III=aug iv=min V=Maj VI=Maj vii=dim
    {2, 8, 9, 2, 1, 1, 8, 0, 0, 0, 0, 0},
};

// Chord type indices: 0=None,1=Maj,2=Min,3=7th,4=Maj7,5=Min7,6=Sus2,7=Sus4,8=Dim,9=Aug
static const BuiltinProg builtinProgs[] = {
    {"Pop",     {{0,1},{4,1},{5,2},{3,1}},                         4}, // I-V-vi-IV
    {"Ballad",  {{0,1},{5,2},{3,1},{4,1}},                         4}, // I-vi-IV-V
    {"Rock",    {{0,1},{3,1},{4,1},{3,1}},                         4}, // I-IV-V-IV
    {"Jazz",    {{1,5},{4,3},{0,4},{5,2}},                         4}, // ii7-V7-Imaj7-vi
    {"Trance",  {{5,2},{3,1},{0,1},{4,1}},                         4}, // vi-IV-I-V
    {"House",   {{0,2},{5,1},{2,1},{6,1}},                         4}, // i-VI-III-VII
    {"Techno",  {{0,2},{2,1},{6,1},{3,2}},                         4}, // i-III-VII-iv
    {"Ambient", {{0,4},{2,5},{5,5},{3,4}},                         4}, // Imaj7-iii7-vi7-IVmaj7
};
static const uint8_t BUILTIN_PROG_COUNT = sizeof(builtinProgs) / sizeof(builtinProgs[0]);

static const char* noteNames[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};


uint8_t MusicTheory::scaleCount() { return SCALE_COUNT; }
uint8_t MusicTheory::chordDefCount() { return CHORD_COUNT; }
uint8_t MusicTheory::builtinProgCount() { return BUILTIN_PROG_COUNT; }

const char* MusicTheory::scaleName(uint8_t idx) {
    return (idx < SCALE_COUNT) ? scales[idx].name : "?";
}

const char* MusicTheory::noteName(uint8_t midiNote) {
    return noteNames[midiNote % 12];
}

char MusicTheory::chordQualChar(uint8_t chordTypeIdx) {
    switch (chordTypeIdx) {
        case 1:  return 'M'; // Major
        case 2:  return 'm'; // Minor
        case 8:  return 'o'; // Dim
        case 9:  return '+'; // Aug
        default: return ' ';
    }
}


uint8_t MusicTheory::totalChordCount() const {
    return CHORD_COUNT + BUILTIN_PROG_COUNT + customProgCount_;
}

bool MusicTheory::addCustomProgression(const char* name, const ProgStep* steps, uint8_t stepCount) {
    if (customProgCount_ >= MAX_CUSTOM_PROGS || stepCount == 0)
        return false;
    CustomProg& cp = customProgs_[customProgCount_];
    strncpy(cp.name, name, 15);
    cp.name[15] = '\0';
    cp.stepCount = (stepCount > MAX_PROG_STEPS) ? MAX_PROG_STEPS : stepCount;
    for (uint8_t i = 0; i < cp.stepCount; i++) {
        cp.steps[i] = steps[i];
        if (cp.steps[i].chordType > 11) cp.steps[i].chordType = 0;
    }
    customProgCount_++;
    return true;
}

const char* MusicTheory::chordName(uint8_t idx) const {
    if (idx < CHORD_COUNT) return chords[idx].name;
    int progIdx = idx - CHORD_PROG_START_IDX;
    if (progIdx >= 0 && progIdx < (int)BUILTIN_PROG_COUNT)
        return builtinProgs[progIdx].name;
    int customIdx = progIdx - BUILTIN_PROG_COUNT;
    if (customIdx >= 0 && customIdx < (int)customProgCount_)
        return customProgs_[customIdx].name;
    return "?";
}

const char* MusicTheory::chordShortName(uint8_t idx) const {
    if (idx < CHORD_COUNT) return chords[idx].shortName;
    return chordName(idx);
}

// rebuildPadMap — MIDI note per pad, from the scale/chord settings.
void MusicTheory::rebuildPadMap(PlaySettings& ps) const {
    const ScaleDef& scale = scales[ps.scaleIndex < SCALE_COUNT ? ps.scaleIndex : 0];
    uint8_t safeScaleIdx = ps.scaleIndex < SCALE_COUNT ? ps.scaleIndex : 0;

    // Base MIDI note: rootNote in the selected octave (octave 0 = C3)
    int baseNote = (int)ps.rootNote + (3 + (int)ps.octave) * 12;

    bool isDiatonic    = (ps.chordIndex == CHORD_DIATONIC_IDX);
    bool isTypes       = (ps.chordIndex == CHORD_TYPES_IDX);
    bool isProgression = (ps.chordIndex >= CHORD_PROG_START_IDX);

    // Get progression data if in progression mode
    const ProgStep* progSteps = nullptr;
    uint8_t progStepCount = 0;
    if (isProgression) {
        int progIdx = ps.chordIndex - CHORD_PROG_START_IDX;
        if (progIdx < (int)BUILTIN_PROG_COUNT) {
            progSteps = builtinProgs[progIdx].steps;
            progStepCount = builtinProgs[progIdx].stepCount;
        } else {
            int customIdx = progIdx - BUILTIN_PROG_COUNT;
            if (customIdx < (int)customProgCount_) {
                progSteps = customProgs_[customIdx].steps;
                progStepCount = customProgs_[customIdx].stepCount;
            }
        }
        if (!progSteps || progStepCount == 0) {
            isProgression = false;
        }
    }

    for (int pad = 0; pad < 16; pad++) {
        int midiNote;
        const ChordDef* padChord;

        if (isProgression) {
            int stepIdx = pad % progStepCount;
            int octaveOffset = pad / progStepCount;
            int degree = progSteps[stepIdx].degree;
            int degreeInScale = degree % scale.count;
            midiNote = baseNote + octaveOffset * 12 + scale.intervals[degreeInScale];
            if (midiNote < 0) midiNote = 0;
            if (midiNote > 127) midiNote = 127;
            uint8_t ct = progSteps[stepIdx].chordType;
            padChord = &chords[ct < 12 ? ct : 0];
            ps.padChordTypeIdx[pad] = ct < 12 ? ct : 0;
        } else if (isTypes) {
            midiNote = baseNote;
            if (midiNote < 0) midiNote = 0;
            if (midiNote > 127) midiNote = 127;
            int typeIdx = pad % 12;
            padChord = &chords[typeIdx];
            ps.padChordTypeIdx[pad] = typeIdx;
        } else {
            int degree = pad;
            int octaveOffset = degree / scale.count;
            int degreeInScale = degree % scale.count;
            midiNote = baseNote + octaveOffset * 12 + scale.intervals[degreeInScale];
            if (midiNote < 0) midiNote = 0;
            if (midiNote > 127) midiNote = 127;

            if (isDiatonic) {
                uint8_t qualIdx = diatonicQuality[safeScaleIdx][degreeInScale];
                padChord = &chords[qualIdx < CHORD_COUNT ? qualIdx : 0];
                ps.padChordTypeIdx[pad] = qualIdx;
            } else {
                uint8_t ci = ps.chordIndex < CHORD_COUNT ? ps.chordIndex : 0;
                padChord = &chords[ci];
                ps.padChordTypeIdx[pad] = ci;
            }
        }

        ps.padMidiMap[pad] = (uint8_t)midiNote;

        ps.padChordCount[pad] = padChord->count;
        for (int c = 0; c < 6; c++) {
            if (c < padChord->count) {
                int cn = midiNote + padChord->intervals[c];
                ps.padChordNotes[pad][c] = (cn <= 127) ? (uint8_t)cn : 0xFF;
            } else {
                ps.padChordNotes[pad][c] = 0xFF;
            }
        }
    }
}

} // namespace crosspad

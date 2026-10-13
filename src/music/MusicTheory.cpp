// SPDX-License-Identifier: GPL-3.0-or-later
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
    /* Appended, not inserted: diatonicQuality is indexed by this array. */
    {"Lydian",        {0,2,4,6,7,9,11,0,0,0,0,0},   7},
    {"Phrygian",      {0,1,3,5,7,8,10,0,0,0,0,0},   7},
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
    /* The ninths. R&B and soul are built on them -- a neo-soul loop written
     * with sevenths is a different record -- and ChordDef has room for six
     * notes, so they cost nothing but their rows. They sit after the twelve
     * so the diatonicQuality tables, which are indices into this array, do
     * not move. */
    {"Maj9",      "M9",   {0,4,7,11,14,0},  5},
    {"Min9",      "m9",   {0,3,7,10,14,0},  5},
    {"Dom9",      "9th",  {0,4,7,10,14,0},  5},
    /* What the rest of a jazz, soul and trap list needs. The thirteenth
     * drops its fifth -- root, third, flat seventh, ninth, thirteenth -- the
     * way it is voiced rather than spelt, because six notes stacked from the
     * root turn to mud down here. */
    {"Min7b5",    "m7b5", {0,3,6,10,0,0},   4},
    {"Dom7b9",    "7b9",  {0,4,7,10,13,0},  5},
    {"Dom13",     "13",   {0,4,10,14,21,0}, 5},
    {"MinMaj7",   "mM7",  {0,3,7,11,0,0},   4},
    {"Min6",      "m6",   {0,3,7,9,0,0},    4},
    {"Diatonic",  "Dia",  {0,0,0,0,0,0},    1},  // special: per-degree chord
    {"Types",     "Typ",  {0,0,0,0,0,0},    1},  // special: each pad = different type
};
static const uint8_t CHORD_COUNT = sizeof(chords) / sizeof(chords[0]);
/* The ones a pad can actually be built from: everything before Diatonic and
 * Types, which are modes rather than chords. */
static const uint8_t CHORD_PLAYABLE = MusicTheory::CHORD_DIATONIC_IDX;

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
    // 9: Lydian: I=Maj II=Maj iii=min #iv=dim V=Maj vi=min vii=min
    {1, 1, 2, 8, 1, 2, 2, 0, 0, 0, 0, 0},
    // 10: Phrygian: i=min II=Maj III=Maj iv=min v=dim VI=Maj vii=min
    {2, 1, 1, 2, 8, 1, 2, 0, 0, 0, 0, 0},
};

// Chord type indices: 0=None,1=Maj,2=Min,3=7th,4=Maj7,5=Min7,6=Sus2,7=Sus4,8=Dim,9=Aug
static const BuiltinProg builtinProgs[] = {
    {"Pop",            {{0,1},{4,1},{5,2},{3,1}}, 4, 1},  // I-V-vi-IV
    {"Ballad",         {{0,1},{5,2},{3,1},{4,1}}, 4, 1},  // I-vi-IV-V
    {"Rock",           {{0,1},{3,1},{4,1},{3,1}}, 4, 1},  // I-IV-V-IV
    {"Jazz",           {{1,5},{4,3},{0,4},{5,2}}, 4, 1},  // ii7-V7-Imaj7-vi
    {"Trance",         {{5,2},{3,1},{0,1},{4,1}}, 4, 1},  // vi-IV-I-V
    {"House",          {{0,2},{5,1},{2,1},{6,1}}, 4, 2},  // i-VI-III-VII
    {"Techno",         {{0,2},{2,1},{6,1},{3,2}}, 4, 2},  // i-III-VII-iv
    {"Ambient",        {{0,4},{2,5},{5,5},{3,4}}, 4, 1},  // Imaj7-iii7-vi7-IVmaj7
    {"Soul Turn",      {{0,4},{5,5},{1,5},{4,3}}, 4, 1},  // Imaj7-vi7-ii7-V7
    {"ii-V-I",         {{1,5},{4,3},{0,4}}, 3, 1},  // ii7-V7-Imaj7
    {"Gospel",         {{3,4},{0,1},{3,4},{0,1}}, 4, 1},  // IVmaj7-I-IVmaj7-I
    {"Deceptive",      {{0,4},{3,4},{4,3},{5,5}}, 4, 1},  // Imaj7-IVmaj7-V7-vi7
    {"Minor Soul",     {{0,5},{3,5},{6,3},{2,3}}, 4, 2},  // i7-iv7-VII7-III7
    {"Tritone",        {{2,5},{1,3},{0,4}}, 3, 0},  // ii7-bII7-Imaj7, degrees are semitones here
    {"Neo-Soul",       {{0,13},{3,12},{6,14},{2,14}}, 4, 2},  // i9-IV9-bVII9-III9
    {"Soul Shuffle",   {{0,12},{3,12},{0,12},{4,14}}, 4, 1},  // I9-IV9-I9-V9
    {"Jazz Turn",      {{1,13},{4,17},{0,12}}, 3, 1},  // Dm9-G13-Cmaj9
    {"Dilla Loop",     {{0,5},{3,5}}, 2, 2},  // Am7-Dm7
    {"Soul Descent",   {{3,4},{2,5},{1,5},{0,4}}, 4, 1},  // Fmaj7-Em7-Dm7-Cmaj7
    {"Minor ii-V-i",   {{1,15},{4,16},{0,13}}, 3, 2},  // Bm7b5-E7b9-Am9
    {"Lo-fi Melanch",  {{3,4},{2,5},{5,5}}, 3, 1},  // Fmaj7-Em7-Am7
    {"Classic Turn",   {{0,4},{5,3},{1,5},{4,3}}, 4, 1},  // Cmaj7-A7-Dm7-G7
    {"Perpetual",      {{1,13},{4,3},{2,5},{5,3}}, 4, 1},  // Dm9-G7-Em7-A7
    {"Cinema Dark",    {{0,2},{5,1},{4,1}}, 3, 2},  // Am-F-E
    {"Epic Weight",    {{0,2},{5,1},{3,2},{4,1}}, 4, 2},  // Fm-Db-Bbm-C
    {"Phrygian",       {{0,2},{1,1}}, 2, 10},  // Em-F
    {"Simple Menace",  {{0,2},{5,1},{4,1}}, 3, 2},  // Cm-Ab-G (same shape as Cinema Dark)
    {"Universal",      {{0,2},{5,1},{2,1},{6,1}}, 4, 2},  // Am-F-C-G
    {"Sad Uplift",     {{0,2},{5,1},{2,1},{6,1}}, 4, 2},  // Dm-Bb-F-C (same shape as Universal)
    {"Gospel Tears",   {{3,4},{3,19},{0,4}}, 3, 1},  // Fmaj7-Fm6-Cmaj7
    {"Andalusian",     {{0,2},{6,1},{5,1},{4,1}}, 4, 2},  // Am-G-F-E
    {"Pop Classic",    {{0,1},{4,1},{5,2},{3,1}}, 4, 1},  // C-G-Am-F (same shape as Pop)
    {"Neo-soul Eb",    {{2,12},{1,5},{0,13},{3,17}}, 4, 2},  // Ebmaj9-Dm7-Cm9-F13
    {"Dorian Vibe",    {{0,5},{3,3}}, 2, 6},  // Dm7-G7
    {"90s R&B",        {{0,12},{5,13},{1,13},{4,17}}, 4, 1},  // Cmaj9-Am9-Dm9-G13
    {"Ballad Pop",     {{0,1},{5,2},{3,1},{4,1}}, 4, 1},  // C-Am-F-G (same shape as Ballad)
    {"Drill",          {{0,2},{5,1},{4,1}}, 3, 2},  // Gm-Eb-D (same shape as Cinema Dark)
    {"Melodic Trap",   {{0,2},{3,2},{5,1},{4,1}}, 4, 2},  // Cm-Fm-Ab-G
    {"Minor Cliche",   {{0,2},{0,18},{0,5},{0,19}}, 4, 2},  // Am-Am(maj7)-Am7-Am6
    {"Backdoor",       {{0,4},{3,5},{6,3},{0,4}}, 4, 7},  // Cmaj7-Fm7-Bb7-Cmaj7
    {"Rising",         {{3,1},{4,1},{5,2}}, 3, 1},  // F-G-Am
    {"Canon",          {{0,1},{4,1},{5,2},{2,2},{3,1},{0,1},{3,1},{4,1}}, 8, 1},  // C-G-Am-Em-F-C-F-G
    {"Church Amen",    {{3,1},{0,1}}, 2, 1},  // F-C, the plagal cadence
    {"Blues",          {{0,3},{3,3},{0,3},{4,3}}, 4, 7},  // C7-F7-C7-G7
    {"Lydian Dream",   {{0,4},{1,1}}, 2, 9},  // Cmaj7-D/C; the bass pedal is not expressible, the II is
    {"Mixo Rock",      {{0,1},{6,1},{3,1},{0,1}}, 4, 7},  // C-Bb-F-C
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
        case 12: return '^'; // Maj9
        case 13: return 'm'; // Min9 -- a pad label has room for one mark, and
                             // minor matters more on it than the ninth does
        case 14: return '9'; // Dom9
        case 15: return 'x'; // Min7b5, the half-diminished
        case 16: return 'b'; // Dom7b9
        case 17: return '3'; // Dom13
        case 18: return 'M'; // MinMaj7 -- the major seventh is what you hear
        case 19: return '6'; // Min6
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

uint8_t MusicTheory::builtinProgScale(uint8_t idx) {
    return idx < BUILTIN_PROG_COUNT ? builtinProgs[idx].scale : 1;
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
            padChord = &chords[ct < CHORD_PLAYABLE ? ct : 0];
            ps.padChordTypeIdx[pad] = ct < CHORD_PLAYABLE ? ct : 0;
        } else if (isTypes) {
            midiNote = baseNote;
            if (midiNote < 0) midiNote = 0;
            if (midiNote > 127) midiNote = 127;
            int typeIdx = pad % CHORD_PLAYABLE;
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

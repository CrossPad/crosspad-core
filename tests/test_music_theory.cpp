#include <catch2/catch_test_macros.hpp>
#include "crosspad/music/MusicTheory.hpp"
#include <cstring>

using namespace crosspad;

TEST_CASE("MusicTheory: scale data", "[music]") {
    REQUIRE(MusicTheory::scaleCount() == 9);

    SECTION("scale names are non-null") {
        for (uint8_t i = 0; i < MusicTheory::scaleCount(); i++) {
            REQUIRE(MusicTheory::scaleName(i) != nullptr);
            REQUIRE(strlen(MusicTheory::scaleName(i)) > 0);
        }
    }

    SECTION("known scales") {
        REQUIRE(std::string(MusicTheory::scaleName(0)) == "Chromatic");
        REQUIRE(std::string(MusicTheory::scaleName(1)) == "Major");
        REQUIRE(std::string(MusicTheory::scaleName(2)) == "Minor");
    }
}

TEST_CASE("MusicTheory: chord data", "[music]") {
    // 14 base chords (indices 0-13, including Diatonic and Types)
    REQUIRE(MusicTheory::chordDefCount() == 22);   // twenty chords, then Diatonic and Types
}

TEST_CASE("MusicTheory: note names", "[music]") {
    REQUIRE(std::string(MusicTheory::noteName(0)) == "C");
    REQUIRE(std::string(MusicTheory::noteName(1)) == "C#");
    REQUIRE(std::string(MusicTheory::noteName(12)) == "C");  // wraps
    REQUIRE(std::string(MusicTheory::noteName(60)) == "C");  // middle C
    REQUIRE(std::string(MusicTheory::noteName(69)) == "A");  // A4
}

TEST_CASE("MusicTheory: rebuildPadMap — chromatic root C", "[music]") {
    MusicTheory mt;
    PlaySettings ps{};
    ps.rootNote = 0;    // C
    ps.octave = 0;      // octave 0 → base MIDI = 0*12 + rootNote
    ps.scaleIndex = 0;  // Chromatic
    ps.chordIndex = 0;  // None (single notes)

    mt.rebuildPadMap(ps);

    SECTION("16 pads get ascending MIDI notes") {
        // Chromatic: each pad = next semitone
        // Pad 0 should be the lowest, ascending
        for (int i = 1; i < 16; i++) {
            REQUIRE(ps.padMidiMap[i] == ps.padMidiMap[i - 1] + 1);
        }
    }

    SECTION("chord count is 1 (single note, no chord)") {
        for (int i = 0; i < 16; i++) {
            REQUIRE(ps.padChordCount[i] == 1);
        }
    }
}

TEST_CASE("MusicTheory: rebuildPadMap — Major scale root C", "[music]") {
    MusicTheory mt;
    PlaySettings ps{};
    ps.rootNote = 0;    // C
    ps.octave = 2;      // C2 → base = 24 + 0 = 24
    ps.scaleIndex = 1;  // Major
    ps.chordIndex = 0;  // None

    mt.rebuildPadMap(ps);

    // Major scale has 7 notes per octave: C D E F G A B
    // With 16 pads we span ~2 octaves
    SECTION("pads follow major scale intervals") {
        // First 7 pads should be: root, +2, +4, +5, +7, +9, +11
        uint8_t base = ps.padMidiMap[0];
        uint8_t expected[] = {0, 2, 4, 5, 7, 9, 11};
        for (int i = 0; i < 7; i++) {
            REQUIRE(ps.padMidiMap[i] - base == expected[i]);
        }
    }
}

TEST_CASE("MusicTheory: rebuildPadMap — Major chords", "[music]") {
    MusicTheory mt;
    PlaySettings ps{};
    ps.rootNote = 0;    // C
    ps.octave = 2;
    ps.scaleIndex = 1;  // Major
    ps.chordIndex = 1;  // Major chord

    mt.rebuildPadMap(ps);

    SECTION("each pad has 3 chord notes (major triad)") {
        for (int i = 0; i < 16; i++) {
            REQUIRE(ps.padChordCount[i] == 3);
        }
    }

    SECTION("first pad chord is root + major third + fifth") {
        uint8_t root = ps.padChordNotes[0][0];
        REQUIRE(ps.padChordNotes[0][1] == root + 4);  // major third
        REQUIRE(ps.padChordNotes[0][2] == root + 7);  // perfect fifth
    }
}

TEST_CASE("MusicTheory: custom progressions", "[music]") {
    MusicTheory mt;

    REQUIRE(mt.customProgCount() == 0);

    ProgStep steps[] = {{0, 1}, {3, 1}, {4, 1}, {0, 1}};
    REQUIRE(mt.addCustomProgression("MyProg", steps, 4));
    REQUIRE(mt.customProgCount() == 1);

    mt.clearCustomProgressions();
    REQUIRE(mt.customProgCount() == 0);
}

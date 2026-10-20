// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "crosspad/music/ChordDesigner.hpp"
#include "crosspad/music/MusicTheory.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace crosspad;
using namespace crosspad::chords;

namespace {

std::vector<int> voiced(Chord c, int octave = 3, uint8_t voicing = Close)
{
    uint8_t out[kMaxNotes] = {};
    const uint8_t n = notes(c, octave, voicing, out);
    return std::vector<int>(out, out + n);
}

} // namespace

TEST_CASE("ChordDesigner: voicing", "[chords]") {
    SECTION("close, from C3, and the inversions") {
        REQUIRE(voiced({0, Maj, 0}) == std::vector<int>{48, 52, 55});
        REQUIRE(voiced({0, Maj, 1}) == std::vector<int>{52, 55, 60});
        REQUIRE(voiced({0, Maj, 2}) == std::vector<int>{55, 60, 64});
    }
    SECTION("open drops the second-highest an octave") {
        // Em7 = E G B D from E3 (52); B goes down to B2
        REQUIRE(voiced({4, Min7, 0}, 3, Open) == std::vector<int>{47, 52, 55, 62});
    }
    SECTION("wide drops the lowest an octave") {
        REQUIRE(voiced({0, Maj, 0}, 3, Wide) == std::vector<int>{36, 52, 55});
    }
    SECTION("five notes: the ninths and the 13") {
        REQUIRE(voiced({7, Dom13, 0}) == std::vector<int>{55, 59, 65, 69, 76});
        REQUIRE(voiced({9, Min9, 0}).size() == 5);
        REQUIRE(qualitySize(Dom9) == kMaxNotes);
    }
    SECTION("clamped into MIDI") {
        for (int n : voiced({11, Dom13, 4}, 9)) REQUIRE(n <= 127);
        for (int n : voiced({0, Maj, 0}, -2, Wide)) REQUIRE(n >= 0);
    }
}

TEST_CASE("ChordDesigner: names", "[chords]") {
    char b[12];
    shortName({4, Min7, 0}, b, sizeof(b));
    REQUIRE(std::string(b) == "Em7");
    shortName({0, Maj, 0}, b, sizeof(b));
    REQUIRE(std::string(b) == "C");
    shortName({10, Maj7, 0}, b, sizeof(b));
    REQUIRE(std::string(b) == "A#maj7");
    REQUIRE(std::string(qualityName(Sus4)) == "sus4");
    REQUIRE(std::string(voicingName(Wide)) == "WIDE");
    REQUIRE(!canSharp(4));            // E
    REQUIRE(!canSharp(11));           // B
    REQUIRE(canSharp(1));             // C#'s natural is C
    REQUIRE(natural(6) == 5);         // F# -> F
}

TEST_CASE("ChordDesigner: presets", "[chords]") {
    REQUIRE(presetCount() == 12);
    REQUIRE(std::string(presetName(0)) == "FUNK");
    REQUIRE(std::string(presetName(11)) == "INIT");
    REQUIRE(std::string(presetName(-1)) == "USER");
    REQUIRE(std::string(presetName(12)) == "USER");
    char b[24];
    presetPreview(0, b, sizeof(b));
    REQUIRE(std::string(b) == "Em7 A9");
    Progression p;
    for (uint8_t i = 0; i < presetCount(); ++i) {
        init(p);
        loadPreset(p, i);
        INFO(presetName((int8_t)i));
        REQUIRE(valid(p));
        REQUIRE(!p.edited);
        REQUIRE(p.preset == (int8_t)i);
    }
}

TEST_CASE("ChordDesigner: edits", "[chords]") {
    Progression p;
    init(p);
    REQUIRE(p.count == 8);
    REQUIRE(p.voicing == Close);
    REQUIRE(!p.edited);

    SECTION("select and the voicing leave it unedited") {
        select(p, 3);
        REQUIRE(p.selected == 3);
        select(p, 8);                        // past the end: ignored
        REQUIRE(p.selected == 3);
        cycleVoicing(p);
        REQUIRE(p.voicing == Open);
        REQUIRE(!p.edited);
    }
    SECTION("root and sharp") {
        setRoot(p, 0);
        REQUIRE(p.edited);
        toggleSharp(p);
        REQUIRE(p.chords[0].root == 1);
        toggleSharp(p);
        REQUIRE(p.chords[0].root == 0);
        setRoot(p, 4);
        toggleSharp(p);                      // E has no sharp
        REQUIRE(p.chords[0].root == 4);
    }
    SECTION("a smaller quality keeps the inversion inside it") {
        setQuality(p, Dom7);
        cycleInversion(p); cycleInversion(p); cycleInversion(p);
        REQUIRE(p.chords[0].inversion == 3);
        setQuality(p, Maj);
        REQUIRE(p.chords[0].inversion == 2);
        cycleInversion(p);
        REQUIRE(p.chords[0].inversion == 0);
    }
    SECTION("add, append, remove, move") {
        select(p, 1);                        // A9
        REQUIRE(add(p));
        REQUIRE(p.count == 9);
        REQUIRE(p.selected == 2);
        REQUIRE(p.chords[2].root == 9);
        REQUIRE(p.chords[3].root == 4);      // what was third is fourth

        REQUIRE(append(p));
        REQUIRE(p.count == 10);
        REQUIRE(p.selected == 9);
        REQUIRE(p.chords[9].quality == p.chords[8].quality);

        REQUIRE(move(p, -1));
        REQUIRE(p.selected == 8);
        REQUIRE(move(p, +1));                // and back again
        select(p, 0);
        REQUIRE(!move(p, -1));

        while (p.count > 1) REQUIRE(remove(p));
        REQUIRE(!remove(p));
        REQUIRE(p.count == 1);
        REQUIRE(p.selected == 0);
    }
    SECTION("sixteen at most") {
        while (p.count < kMaxChords) REQUIRE(append(p));
        REQUIRE(!append(p));
        REQUIRE(!add(p));
        REQUIRE(valid(p));
    }
}

TEST_CASE("ChordDesigner: a stored copy is checked before it is used", "[chords]") {
    Progression p;
    init(p);
    REQUIRE(valid(p));
    Progression bad = p;
    bad.count = 0;
    REQUIRE(!valid(bad));
    bad = p; bad.count = 17;
    REQUIRE(!valid(bad));
    bad = p; bad.selected = p.count;
    REQUIRE(!valid(bad));
    bad = p; bad.chords[2].quality = QualityCount;
    REQUIRE(!valid(bad));
    bad = p; bad.chords[0].inversion = 3;    // Em7 has four notes: 0..3 is fine
    REQUIRE(valid(bad));
    bad.chords[0].inversion = 4;
    REQUIRE(!valid(bad));
    bad = p; bad.voicing = VoicingCount;
    REQUIRE(!valid(bad));
    bad = p; bad.preset = 12;
    REQUIRE(!valid(bad));
    std::memset(&bad, 0xFF, sizeof(bad));    // erased flash
    REQUIRE(!valid(bad));
}

TEST_CASE("MusicTheory: rebuildPadMap leaves no pad silent", "[music][chords]") {
    MusicTheory mt;
    PlaySettings ps{};
    ps.padSilent = 0xFFFF;
    mt.rebuildPadMap(ps);
    REQUIRE(ps.padSilent == 0);
}

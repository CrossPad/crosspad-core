// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file ChordDesigner.hpp
 * @brief A progression of chords spelled out -- a root, a quality, an
 *        inversion each -- for the pads to play, one chord a pad.
 *
 * Pitch Play's CHORD (UX 2000 "Chord Designer"). Not degrees of a scale, the
 * way MusicTheory's progressions are: a progression in a genre is a record of
 * particular chords ("Em7 A9 Em7 A9 Cmaj7 B7 ..."), and the player edits
 * those -- the root on seven keys and a sharp, the quality on ten, the
 * inversion, the order -- and hears each change at once. Up to sixteen
 * chords, pad n playing chord n; a voicing (close, open, wide) for all of
 * them. Twelve genre presets to start from, the player's edits marked as such.
 *
 * Pure data and arithmetic: nothing here plays a note or knows a pad. The
 * caller voices a chord with notes() and plays what comes back.
 */

#include <cstddef>
#include <cstdint>

namespace crosspad::chords {

constexpr uint8_t kMaxChords = 16;   ///< one a pad
constexpr uint8_t kMaxNotes  = 5;    ///< the widest quality: the ninths and the 13

enum Quality : uint8_t { Maj, Min, Dom7, Maj7, Min7, Dom9, Min9, Dom13, Dim, Sus4, QualityCount };
enum Voicing : uint8_t { Close, Open, Wide, VoicingCount };

struct Chord {
    uint8_t root;        ///< pitch class, C = 0
    uint8_t quality;     ///< Quality
    uint8_t inversion;   ///< 0 .. qualitySize() - 1
};

/// Plain data, so a caller can store it as it is (and must check it with
/// valid() when it reads it back).
struct Progression {
    Chord   chords[kMaxChords];
    uint8_t count;       ///< 1 .. kMaxChords
    uint8_t selected;    ///< the chord being edited
    uint8_t voicing;     ///< Voicing, for every chord
    int8_t  preset;      ///< what it was loaded from, -1 = the player's own
    bool    edited;      ///< changed since it was loaded
};

// ── names ───────────────────────────────────────────────────────────────────
const char* noteName(uint8_t root);       ///< "C", "C#" ...
const char* qualityName(uint8_t q);       ///< "maj", "m7" ... -- what the keys say
const char* qualitySuffix(uint8_t q);     ///< "", "m", "7" ... -- what follows a root
const char* voicingName(uint8_t v);       ///< "CLOSE", "OPEN", "WIDE"
void shortName(const Chord& c, char* buf, size_t n);   ///< "Em7", "C", "Am"
uint8_t qualitySize(uint8_t q);           ///< notes in it, 3 .. kMaxNotes

bool    isSharp(uint8_t root);
uint8_t natural(uint8_t root);            ///< C# -> C
bool    canSharp(uint8_t root);           ///< false for E and B

/**
 * The chord as MIDI notes, ascending, into @p out; returns how many.
 * @p octave is the C the root counts from: 3 puts a C chord's root on 48 (C3).
 * Inversions lift the lowest note an octave, one at a time; Open drops the
 * second-highest an octave, Wide the lowest. Notes are clamped to 0..127.
 */
uint8_t notes(const Chord& c, int octave, uint8_t voicing, uint8_t out[kMaxNotes]);

// ── presets ─────────────────────────────────────────────────────────────────
uint8_t     presetCount();
const char* presetName(int8_t idx);       ///< -1 (or out of range) -> "USER"
void        presetPreview(uint8_t idx, char* buf, size_t n);   ///< its first two chords

// ── edits: all but load(), select() and the voicing mark the copy edited ────
void init(Progression& p);                ///< the first preset, close voicing
void loadPreset(Progression& p, uint8_t idx);
void select(Progression& p, uint8_t i);
void setRoot(Progression& p, uint8_t root);
void toggleSharp(Progression& p);         ///< the selected root and its sharp; E and B have none
void setQuality(Progression& p, uint8_t q);   ///< the inversion is kept inside the new size
void cycleInversion(Progression& p);
void cycleVoicing(Progression& p);
bool add(Progression& p);                 ///< a copy of the selected after it, selected
bool append(Progression& p);              ///< a copy of the last at the end, selected
bool remove(Progression& p);              ///< the selected; one is always left
bool move(Progression& p, int dir);       ///< swap the selected with its neighbour, -1 or +1

/// Everything in range: what a stored copy has to pass before it is used.
bool valid(const Progression& p);

} // namespace crosspad::chords

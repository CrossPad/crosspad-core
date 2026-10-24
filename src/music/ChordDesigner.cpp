// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/music/ChordDesigner.hpp"

#include <cstdio>
#include <cstring>

namespace crosspad::chords {
namespace {

const char* const kNotes[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char* const kQualityName[QualityCount]   = {"maj", "min", "7", "maj7", "m7", "9", "m9", "13", "dim", "sus4"};
const char* const kQualitySuffix[QualityCount] = {"", "m", "7", "maj7", "m7", "9", "m9", "13", "dim", "sus4"};
const char* const kVoicing[VoicingCount] = {"CLOSE", "OPEN", "WIDE"};

/// Intervals from the root, and how many of them.
const uint8_t kIntervals[QualityCount][kMaxNotes] = {
    {0, 4, 7},         {0, 3, 7},         {0, 4, 7, 10},      {0, 4, 7, 11},      {0, 3, 7, 10},
    {0, 4, 7, 10, 14}, {0, 3, 7, 10, 14}, {0, 4, 10, 14, 21}, {0, 3, 6},          {0, 5, 7},
};
const uint8_t kSize[QualityCount] = {3, 3, 4, 4, 4, 5, 5, 5, 3, 3};

enum : uint8_t { C_, Cs, D_, Ds, E_, F_, Fs, G_, Gs, A_, As, B_ };

struct Preset {
    const char* name;
    uint8_t     count;
    Chord       chords[kMaxChords];
};

#define CH(r, q) Chord{r, q, 0}
const Preset kPresets[] = {
    {"FUNK",     8, {CH(E_, Min7), CH(A_, Dom9), CH(E_, Min7), CH(A_, Dom9), CH(C_, Maj7), CH(B_, Dom7),
                     CH(E_, Min7), CH(A_, Dom13)}},
    {"SOUL",     5, {CH(Cs, Maj7), CH(C_, Dom7), CH(F_, Min7), CH(Ds, Min7), CH(Gs, Dom7)}},
    {"NEO-SOUL", 6, {CH(F_, Maj7), CH(E_, Min7), CH(D_, Min9), CH(C_, Maj7), CH(As, Maj7), CH(A_, Dom7)}},
    {"R&B",      4, {CH(C_, Maj7), CH(A_, Min9), CH(D_, Min9), CH(G_, Dom13)}},
    {"GOSPEL",   9, {CH(C_, Maj7), CH(E_, Dom7), CH(A_, Min7), CH(D_, Dom9), CH(G_, Dom13), CH(F_, Maj7),
                     CH(F_, Min7), CH(C_, Maj7), CH(G_, Sus4)}},
    {"JAZZ",     7, {CH(D_, Min7), CH(G_, Dom7), CH(C_, Maj7), CH(A_, Dom7), CH(D_, Min9), CH(G_, Dom13),
                     CH(C_, Maj7)}},
    {"BLUES",   12, {CH(C_, Dom7), CH(F_, Dom7), CH(C_, Dom7), CH(C_, Dom7), CH(F_, Dom7), CH(F_, Dom7),
                     CH(C_, Dom7), CH(C_, Dom7), CH(G_, Dom7), CH(F_, Dom7), CH(C_, Dom7), CH(G_, Dom7)}},
    {"LO-FI",    4, {CH(A_, Min9), CH(D_, Dom9), CH(G_, Maj7), CH(C_, Maj7)}},
    {"HOUSE",    5, {CH(C_, Min9), CH(F_, Dom9), CH(C_, Min9), CH(Gs, Maj7), CH(G_, Dom7)}},
    {"DISCO",    6, {CH(A_, Min7), CH(D_, Dom9), CH(A_, Min7), CH(D_, Dom9), CH(F_, Maj7), CH(E_, Dom7)}},
    {"POP",      4, {CH(C_, Maj), CH(G_, Maj), CH(A_, Min), CH(F_, Maj)}},
    {"INIT",     1, {CH(C_, Maj)}},
};
#undef CH
constexpr uint8_t kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

/// Ascending, in place: insertion sort, five numbers at most.
void sort(int* a, uint8_t n)
{
    for (uint8_t i = 1; i < n; ++i) {
        const int v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; --j; }
        a[j + 1] = v;
    }
}

Chord& cur(Progression& p) { return p.chords[p.selected]; }

} // namespace

const char* noteName(uint8_t root) { return kNotes[root % 12]; }
const char* qualityName(uint8_t q) { return q < QualityCount ? kQualityName[q] : "?"; }
const char* qualitySuffix(uint8_t q) { return q < QualityCount ? kQualitySuffix[q] : ""; }
const char* voicingName(uint8_t v) { return v < VoicingCount ? kVoicing[v] : "?"; }
uint8_t qualitySize(uint8_t q) { return q < QualityCount ? kSize[q] : 1; }

void shortName(const Chord& c, char* buf, size_t n)
{
    std::snprintf(buf, n, "%s%s", noteName(c.root), qualitySuffix(c.quality));
}

bool isSharp(uint8_t root) { return kNotes[root % 12][1] == '#'; }
uint8_t natural(uint8_t root) { return isSharp(root) ? (uint8_t)(root % 12 - 1) : (uint8_t)(root % 12); }
bool canSharp(uint8_t root)
{
    const uint8_t n = natural(root);
    return n != E_ && n != B_;
}

uint8_t notes(const Chord& c, int octave, uint8_t voicing, uint8_t out[kMaxNotes])
{
    const uint8_t q = c.quality < QualityCount ? c.quality : (uint8_t)Maj;
    const uint8_t n = kSize[q];
    int m[kMaxNotes];
    const int base = 12 * (octave + 1) + c.root % 12;
    for (uint8_t i = 0; i < n; ++i) m[i] = base + kIntervals[q][i];
    sort(m, n);
    for (uint8_t k = 0; k < c.inversion && k < n; ++k) { m[0] += 12; sort(m, n); }
    if (voicing == Open && n >= 3) { m[n - 2] -= 12; sort(m, n); }
    if (voicing == Wide) m[0] -= 12;   // the lowest, lower still: stays the lowest
    for (uint8_t i = 0; i < n; ++i) out[i] = (uint8_t)(m[i] < 0 ? 0 : (m[i] > 127 ? 127 : m[i]));
    return n;
}

uint8_t presetCount() { return kPresetCount; }
const char* presetName(int8_t idx) { return idx < 0 || idx >= (int8_t)kPresetCount ? "USER" : kPresets[idx].name; }

void presetPreview(uint8_t idx, char* buf, size_t n)
{
    if (idx >= kPresetCount) { if (n) buf[0] = '\0'; return; }
    char a[12], b[12] = "";
    shortName(kPresets[idx].chords[0], a, sizeof(a));
    if (kPresets[idx].count > 1) shortName(kPresets[idx].chords[1], b, sizeof(b));
    std::snprintf(buf, n, "%s %s", a, b);
}

void init(Progression& p)
{
    std::memset(&p, 0, sizeof(p));
    p.voicing = Close;
    loadPreset(p, 0);
}

void loadPreset(Progression& p, uint8_t idx)
{
    if (idx >= kPresetCount) return;
    std::memcpy(p.chords, kPresets[idx].chords, sizeof(p.chords));
    p.count = kPresets[idx].count;
    p.selected = 0;
    p.preset = (int8_t)idx;
    p.edited = false;
}

void select(Progression& p, uint8_t i) { if (i < p.count) p.selected = i; }

void setRoot(Progression& p, uint8_t root)
{
    cur(p).root = root % 12;
    p.edited = true;
}

void toggleSharp(Progression& p)
{
    Chord& c = cur(p);
    if (isSharp(c.root)) --c.root;
    else if (canSharp(c.root)) ++c.root;
    else return;
    p.edited = true;
}

void setQuality(Progression& p, uint8_t q)
{
    if (q >= QualityCount) return;
    Chord& c = cur(p);
    c.quality = q;
    if (c.inversion >= kSize[q]) c.inversion = kSize[q] - 1;
    p.edited = true;
}

void cycleInversion(Progression& p)
{
    Chord& c = cur(p);
    c.inversion = (uint8_t)((c.inversion + 1) % qualitySize(c.quality));
    p.edited = true;
}

void cycleVoicing(Progression& p) { p.voicing = (uint8_t)((p.voicing + 1) % VoicingCount); }

bool add(Progression& p)
{
    if (p.count >= kMaxChords) return false;
    std::memmove(&p.chords[p.selected + 2], &p.chords[p.selected + 1],
                 (size_t)(p.count - p.selected - 1) * sizeof(Chord));
    p.chords[p.selected + 1] = cur(p);
    ++p.count;
    ++p.selected;
    p.edited = true;
    return true;
}

bool append(Progression& p)
{
    if (p.count >= kMaxChords || p.count == 0) return false;
    p.chords[p.count] = p.chords[p.count - 1];
    p.selected = p.count++;
    p.edited = true;
    return true;
}

bool remove(Progression& p)
{
    if (p.count <= 1) return false;
    std::memmove(&p.chords[p.selected], &p.chords[p.selected + 1],
                 (size_t)(p.count - p.selected - 1) * sizeof(Chord));
    --p.count;
    if (p.selected >= p.count) p.selected = p.count - 1;
    p.edited = true;
    return true;
}

bool move(Progression& p, int dir)
{
    const int t = (int)p.selected + dir;
    if (t < 0 || t >= p.count) return false;
    const Chord c = p.chords[t];
    p.chords[t] = cur(p);
    cur(p) = c;
    p.selected = (uint8_t)t;
    p.edited = true;
    return true;
}

bool valid(const Progression& p)
{
    if (p.count < 1 || p.count > kMaxChords || p.selected >= p.count) return false;
    if (p.voicing >= VoicingCount || p.preset < -1 || p.preset >= (int8_t)kPresetCount) return false;
    for (uint8_t i = 0; i < p.count; ++i) {
        const Chord& c = p.chords[i];
        if (c.root >= 12 || c.quality >= QualityCount || c.inversion >= kSize[c.quality]) return false;
    }
    return true;
}

} // namespace crosspad::chords

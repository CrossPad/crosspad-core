// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

namespace crosspad {

/// Where to cut a loop. Pure arithmetic on a level envelope (see
/// WaveOverview), so a threshold turned on the encoder answers at once
/// instead of reading the card again.
namespace SliceMath {

/// Envelope steps: 0 is silence (-kEnvRangeDb dBFS and below), 255 full scale.
constexpr float   kEnvRangeDb    = 90.0f;
constexpr float   kEnvDbPerStep  = kEnvRangeDb / 255.0f;
constexpr uint8_t kMaxSlices     = 16;

/// The envelope value for a block's mean square (of samples scaled to +-1).
uint8_t envelopeStep(float meanSquare);

/// Slices at the hits between @p start and @p end: a hit is a rise of the
/// envelope from one hop to the next, and @p sensitivity (0..100) sets how
/// small a rise counts (100 = 3 dB, 0 = 30 dB). Hits closer than
/// @p minGapFrames to the one before are dropped, and when there are more
/// than the pads can hold the strongest are kept. The first slice always
/// starts at @p start. @p envStart is the frame the envelope begins at.
std::vector<uint32_t> onsets(const std::vector<uint8_t>& envelope, uint32_t hop, uint32_t envStart,
                             uint32_t start, uint32_t end, int sensitivity, uint32_t minGapFrames,
                             uint8_t maxSlices = kMaxSlices);

/// Where a take sounds: @c start at the foot of its first real transient,
/// @c end where it has fallen back to the room.
struct TrimPoints {
    uint32_t start = 0;
    uint32_t end   = 0;
};

/// The sounding part of a take, from its level envelope (see WaveOverview).
///
/// Written for a sample going under a pad: what must sound the instant the pad
/// is struck is the transient, so @c start is the attack itself and carries no
/// pad of silence in front of it.
///
/// The gate between the room and the sound is found on the histogram of the
/// envelope (Otsu's threshold), not counted up from the floor: a room is not
/// steady, and one measured here spread 10 dB above its own quietest hop, so a
/// floor-anchored gate was still room and the start sat in it. From the gate
/// crossing a *transient* is looked for: a jump of kTrimRiseDb from one hop to
/// the next, landing kTrimRiseDb clear of the gate, and standing kTrimSkipDb
/// above everything between it and the crossing. When none of that is met (a
/// bowed note, a pad swelling in) the gate crossing stands. @c start is then one hop before that point and no
/// further: the attack began in the hop the rise does not show yet, and any
/// rule that reads the level to walk back instead walks through a living room
/// hop by hop, which is how this once put the start a fifth of a second before
/// the hit. The end is the last hop above the gate, followed while the tail is
/// still clearly above the floor, plus kTrimTailMs so it is not cut square.
///
/// A take with nothing to find -- empty, silent, or one unbroken level, where
/// a gate would cut music rather than silence -- comes back as [0, @p frames):
/// the ends, which is what the markers do without this.
TrimPoints autoTrim(const std::vector<uint8_t>& envelope, uint32_t hop, uint32_t envStart,
                    uint32_t frames, uint32_t sampleRate);

/// The quietest part of a take that is taken for its noise floor, percent.
constexpr int   kTrimFloorPct = 10;
/// The least the gate may sit above the floor, whatever the histogram says.
constexpr float kTrimGateDb   = 9.0f;
/// A take whose loudest part is not this far above its floor has no silence to
/// trim -- only music that happens to be quiet, and gating that cuts the music
/// in half. The line is drawn from measurement: real microphone takes carry
/// 53-63 dB between their peak and their floor, and a hit decaying into digital
/// silence 69 dB, while dense percussive samples that sound to their last frame
/// carry 13-32 dB. Nothing measured lands between 32 and 53.
constexpr float kTrimSpanDb   = 40.0f;
/// A jump this big from one hop to the next is an attack, not a room.
constexpr float kTrimRiseDb   = 12.0f;
/// ...and stands this far above everything between it and the gate, so a hit
/// after a breath is found while a note fading up is not cut into.
constexpr float kTrimSkipDb   = 20.0f;
/// How far under the gate a tail is still followed. The gate sits in the gap
/// between room and sound, so a few dB below it is still sound.
constexpr float kTrimTailDb   = 6.0f;
/// Kept after the last sound so a tail is not cut square. Nothing is kept
/// before the attack: the one hop the start steps back is already as much
/// silence as a pad should carry.
constexpr uint16_t kTrimTailMs = 20;

/// @p count slices of equal length between @p start and @p end.
std::vector<uint32_t> equal(uint32_t start, uint32_t end, uint8_t count);

/// Tempo of @p frames as @p bars bars of 4/4 at @p sampleRate; 0 when unknown.
float bpmOf(uint32_t frames, uint32_t sampleRate, uint8_t bars);

/// A slice's edges, against clicks: it always rises over at least
/// kMinFadeInFrames and falls over its last kFadeOutFrames.
constexpr uint32_t kMinFadeInFrames = 32;
constexpr uint32_t kFadeOutFrames   = 256;
constexpr uint16_t kMaxAttackMs     = 500;

/// The rise of a slice with an attack of @p attackMs, in frames: never
/// shorter than kMinFadeInFrames, never longer than kMaxAttackMs.
uint32_t attackFrames(uint16_t attackMs, uint32_t sampleRate);

/// The gain of frame @p rel of a slice @p len frames long: a straight rise
/// over @p attack frames, a fall over the last kFadeOutFrames, the lower of
/// the two. What a kit's slice is written with and what its audition plays.
float sliceGain(uint32_t rel, uint32_t len, uint32_t attack);

/// Transposing is varispeed, as on a turntable: higher is also shorter.
constexpr int kMaxSemitones = 12;

/// Source frames per played frame for @p semitones (clamped to
/// +-kMaxSemitones): 2^(semitones/12).
double pitchRatio(int semitones);

/// How many frames @p srcFrames of source last played at @p ratio.
uint32_t pitchedFrames(uint32_t srcFrames, double ratio);

/// @p n stereo frames played at @p ratio into @p out: played frame k is the
/// source at @p pos + k * @p ratio, cubic Hermite between frames. @p src is a
/// window of the source, @p srcFrames long, and @p pos counts from its first
/// frame; taps past its edges read its edge frames, so a slice read through a
/// window of itself never hears its neighbours.
void readPitched(const int16_t* src, uint32_t srcFrames, double pos, double ratio,
                 int16_t* out, uint32_t n);

} // namespace SliceMath
} // namespace crosspad

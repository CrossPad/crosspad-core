// SPDX-License-Identifier: MIT
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

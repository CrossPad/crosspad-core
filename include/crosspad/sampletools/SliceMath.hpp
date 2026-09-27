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

} // namespace SliceMath
} // namespace crosspad

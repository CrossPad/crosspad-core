// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace crosspad {

/**
 * @brief The song engine's ring and card-access sizes, in stereo int16 frames
 *        (4 bytes each).
 *
 * A platform tunes them to its memory and storage through
 * ISongEnginePlatform::sizes(), read once when the engine is built. The
 * defaults are the ones the ESP32-S3 board runs.
 *
 * The tracks are mixed by the worker into one ring, not played from a ring
 * each, so a mute or a track fader is heard a song ring's depth late.
 */
struct SongEngineSizes {
    /// A bounce's ring: the card stall a bounce rides out (128 kB, ~0.7 s).
    /// Never less than the 8192 frames a bounce starts with.
    uint32_t bounceRingFrames = 32768;
    /// The ring the worker mixes the tracks into, ahead of playback (64 kB,
    /// ~370 ms). Never less than the 4096 frames playback primes before the
    /// first note.
    uint32_t songRingFrames = 16384;
    /// One card access (4 kB): a bounce hands ISongEnginePlatform::writeIo()
    /// pieces of this size, each ending on a multiple of it in the file.
    uint32_t chunkFrames = 1024;
};

} // namespace crosspad

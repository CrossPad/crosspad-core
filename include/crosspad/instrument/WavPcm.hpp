// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file WavPcm.hpp
 * @brief Minimal RIFF/WAVE reader for instrument zones.
 *
 * 16-bit PCM only, mono or stereo; anything else is reported as unsupported.
 * Chunks may appear in any order and unknown ones (LIST, fact, ...) are
 * skipped, honouring RIFF's odd-size padding byte.
 *
 * Reads through a 4 kB chunk buffer — the whole file is never held in memory,
 * which matters on a board with a few tens of kB of internal RAM to spare.
 *
 * Declared chunk sizes are treated as untrusted input: every one is clamped to
 * the bytes the file actually holds, so a truncated or corrupt WAV reports the
 * frames that are there instead of the frames it claims.
 */

#include <cstdint>

namespace crosspad {

struct WavInfo {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t frames = 0;
};

/**
 * @brief Read the header of a WAV file.
 * @param out filled with whatever was parsed, even when the format is rejected.
 * @return false if the file is unreadable, not RIFF/WAVE, or not 16-bit
 *         mono/stereo PCM.
 */
bool wavReadInfo(const char* path, WavInfo& out);

/**
 * @brief Read @p frames frames as mono 16-bit PCM; stereo is averaged.
 *
 * A file shorter than @p frames leaves the remainder zeroed.
 * @return false on the same conditions as wavReadInfo().
 */
bool wavReadMono16(const char* path, int16_t* out, uint32_t frames);

} // namespace crosspad

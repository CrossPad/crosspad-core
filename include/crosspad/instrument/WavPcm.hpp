// SPDX-License-Identifier: GPL-3.0-or-later
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

#include "crosspad/audio/WavLayout.hpp"

#include <cstdint>
#include <cstdio>

namespace crosspad {

struct WavInfo {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t frames = 0;
};

/**
 * @brief An open WAV, read a piece at a time.
 *
 * The whole-file helpers below are wrappers over this, so there is one copy of
 * the chunk walking. A zone longer than the bank budget is loaded through this
 * class rather than read whole and then cut: peak memory is the destination
 * plus the 4 kB chunk buffer, whatever the file's length.
 *
 * One file per instance, no copies; close() is idempotent and the destructor
 * calls it.
 */
class WavPcmReader {
public:
    WavPcmReader() = default;
    ~WavPcmReader() { close(); }

    WavPcmReader(const WavPcmReader&) = delete;
    WavPcmReader& operator=(const WavPcmReader&) = delete;

    /**
     * @brief Open @p path and parse its header.
     * @return false if the file is unreadable, not RIFF/WAVE, or not 16-bit
     *         mono/stereo PCM. Nothing is left open on failure.
     */
    bool open(const char* path);

    /// Valid after a successful open(); zeroed before one.
    const WavInfo& info() const { return info_; }

    /**
     * @brief Read the next @p frames frames as mono 16-bit PCM; stereo averaged.
     *
     * @return frames actually written, 0 at the end of the data chunk. A short
     *         count is the end of the file, not an error — the caller decides
     *         whether the remainder of its buffer should be zeroed.
     */
    uint32_t readMono16(int16_t* out, uint32_t frames);

    void close();

private:
    /// Drop the file and its chunk buffer, keeping whatever info() reports.
    void release();

    FILE* f_ = nullptr;
    WavInfo info_{};
    uint8_t* buf_ = nullptr;
    uint32_t left_ = 0;        ///< frames still unread in the data chunk
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

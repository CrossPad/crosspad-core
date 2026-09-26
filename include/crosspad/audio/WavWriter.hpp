// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <cstdio>

namespace crosspad {

/// Writes 16-bit PCM WAV, streaming: the header goes out first with the sizes
/// left at zero, frames are appended as they come, and close() goes back and
/// fills the sizes in.
///
/// Sizes-last rather than a length known up front, because the writer of a
/// recording rarely knows how long it will be -- and a file whose close()
/// never ran (card pulled, board reset) still opens in every editor that
/// reads the data chunk to the end of the file, which is most of them.
///
/// Portable: plain stdio, so the simulator writes the same files.
class WavWriter {
public:
    WavWriter() = default;
    ~WavWriter() { close(); }
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    bool open(const char* path, uint32_t sampleRate, uint16_t channels);
    /// Grow the file to hold @p frames now, so the writes that follow only
    /// fill space the filesystem already gave out. For a take that must not
    /// wait: finding free clusters on a big card is a scan of its FAT, and
    /// that is hundreds of milliseconds when it lands mid-recording.
    bool reserve(uint32_t frames);
    /// Append @p frames interleaved frames (frames * channels samples).
    bool write(const int16_t* interleaved, uint32_t frames);
    /// Patch the sizes and close. Safe to call twice; false if the sizes
    /// could not be written back.
    bool close();

    bool     isOpen() const { return file_ != nullptr; }
    uint32_t frames() const { return frames_; }

private:
    bool writeHeader(uint32_t dataBytes);

    FILE*    file_       = nullptr;
    uint32_t sampleRate_ = 0;
    uint16_t channels_   = 0;
    uint32_t frames_     = 0;
};

} // namespace crosspad

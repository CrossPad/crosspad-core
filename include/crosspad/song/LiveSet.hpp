// SPDX-License-Identifier: MIT
#pragma once

#include "crosspad/song/AudioSong.hpp"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace crosspad {

/// A clip file as an export measured it. frames == 0: missing, empty or not
/// a WAV the engine plays -- its blocks are left out and the report says so.
struct LiveSetClipFile {
    std::string file;          ///< name inside the project's clips/
    uint32_t    frames = 0;
    uint32_t    sampleRate = 0;
    uint64_t    bytes = 0;     ///< the file's size, which Live's FileRef records
    int64_t     modified = 0;  ///< unix seconds
};

/// What an export placed, and what it left out or had to shorten.
struct LiveSetReport {
    uint16_t clips = 0;
    std::vector<std::string> notes;   ///< one line each
};

/// An AudioSong as an Ableton Live 12 Set: four audio tracks named Track 1-4,
/// each block an arrangement clip on the bars it sits on, at the song's
/// tempo and metre, with the track's fader and mute and the song's loop.
///
/// A clip plays the way the song engine plays it: once, from its first
/// sample, at the speed it was recorded -- warped to its own length at the
/// song's tempo, so it is never stretched -- and cut where the block ends, a
/// later block starts, or the song ends. A file shorter than its block ends
/// early. Nothing is resampled or copied: a clip refers to clips/<file>
/// relative to the project, so the song's folder is the Live project.
///
/// @p skeleton is crosspad-core/data/live12_set.xml.gz uncompressed: Live's
/// empty set with one AudioTrack, cloned once per song track. Ids Live keeps
/// in one namespace (a track's, a Pointee's, anything ending in Target) are
/// renumbered per clone; every other Id is local and kept. It is edited in
/// place, so hand it over with std::move: it is the largest thing an export
/// holds.
///
/// The XML goes to @p out a piece at a time, never whole. False when the
/// skeleton is not the one expected or @p out refused a piece.
bool writeLiveSetXml(std::string skeleton, const AudioSong& song,
                     const std::vector<LiveSetClipFile>& files,
                     const std::function<bool(const char*, size_t)>& out,
                     LiveSetReport& report);

/// A .gz file of stored (uncompressed) deflate blocks: what Live reads a Set
/// from, written in pieces with no compressor's memory behind it.
class GzipStoredWriter {
public:
    GzipStoredWriter() = default;
    ~GzipStoredWriter() { if (f_) std::fclose(f_); }
    GzipStoredWriter(const GzipStoredWriter&) = delete;
    GzipStoredWriter& operator=(const GzipStoredWriter&) = delete;

    bool open(const std::string& path);
    bool write(const char* data, size_t n);
    /// The last block, the CRC and the length. False when anything written
    /// since open() did not reach the file.
    bool close();

private:
    std::FILE* f_ = nullptr;
    uint32_t   crc_ = 0;
    uint32_t   size_ = 0;
    bool       ok_ = false;
};

} // namespace crosspad

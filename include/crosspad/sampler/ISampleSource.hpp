// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file ISampleSource.hpp
 * @brief Where sample bytes come from, as the engine sees it.
 *
 * The engine never opens a file, never knows a mount point and never learns
 * which thread may touch the card. It asks for frames of a path and is given
 * int16 back. Everything the platform has learned about the card -- the open
 * handle cache, the descriptor ceiling, which FreeRTOS task may read, the
 * pacing against a kit load -- stays on the platform's side of this
 * interface, where it already is.
 *
 * Addressing is by frame, not by byte, and absolute, not relative to a
 * position the engine has to keep in step with. A streaming engine issues
 * reads out of order (one voice is at 3 s while another starts at 0), and a
 * seek-then-read pair is only atomic if the caller holds a lock across both.
 * One call that carries its own offset needs no such lock.
 *
 * read() is called from the streaming thread and from the loader, never from
 * the render thread. It is allowed to block; the engine is written so that
 * nothing audible waits for it.
 */

#include <cstdint>

namespace crosspad {

/// What a file holds, as read from its header.
struct SampleDesc {
    uint32_t frames     = 0;   ///< per channel
    uint32_t sampleRate = 0;   ///< the file's own rate; the engine transposes rather than resamples
    uint8_t  channels   = 0;   ///< 1 or 2; more is refused
    uint8_t  bits       = 0;   ///< as stored. read() always hands back int16.
    bool valid() const { return frames != 0 && sampleRate != 0 && (channels == 1 || channels == 2); }
};

class ISampleSource {
public:
    virtual ~ISampleSource() = default;

    /// The file's header. False when it is missing, unreadable or a format
    /// the platform cannot convert. Loader thread.
    virtual bool describe(const char* path, SampleDesc& out) = 0;

    /// Frames [first, first + frames) of @p path, interleaved, int16, at the
    /// file's own channel count -- so @p dst holds frames * channels samples.
    /// Short reads are the end of the file, not an error.
    ///
    /// @return frames actually written.
    virtual uint32_t read(const char* path, uint32_t first, uint32_t frames, int16_t* dst) = 0;

    /// A hint that nothing will read @p path for a while, so a handle held
    /// for it may be given back. Optional; a platform with no cache ignores it.
    virtual void done(const char* path) { (void)path; }
};

} // namespace crosspad

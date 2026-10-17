// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file WavLayout.hpp
 * @brief A RIFF/WAVE file as the sampler reads it: one parser for the engine
 *        and for anything that must agree with it.
 *
 * The native engine's describe() and the kit rate converter both read their
 * headers here, so a file one of them plays is exactly a file the other
 * accepts, with the same frames at the same offset. The rules are the
 * engine's, which were written down first:
 *
 *  - "RIFF"/"WAVE"; at most kMaxChunkHeaders chunk headers are walked to
 *    find the data;
 *  - a `fmt ` of 16 bytes or more, before the `data`;
 *  - the format tag is not looked at: 16-bit samples are played as PCM
 *    whatever it says (pcm16());
 *  - the data runs to its stated size or the end of the file, whichever
 *    comes first -- a read past the end is the end of the sample.
 *
 * Past the data the walk goes on, for the metadata chunks a rewrite of the
 * file carries over; nothing there changes what is played.
 */

#include <cstdint>
#include <cstdio>

namespace crosspad {

struct WavLayout {
    struct Chunk {
        char     id[4]     = {};
        uint32_t offset    = 0;       ///< of the payload
        uint32_t size      = 0;       ///< of the payload, without the pad byte
        bool     afterData = false;
    };

    static constexpr int kMaxChunkHeaders = 64;

    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    uint32_t dataOffset = 0;   ///< of the first sample
    uint32_t dataBytes  = 0;   ///< clamped to what the file holds
    uint32_t frames     = 0;
    uint32_t fileBytes  = 0;
    uint32_t riffBytes  = 0;   ///< the RIFF size field plus its 8-byte header
    /// Chunks written to the caller's list (parse()): everything but `fmt `
    /// and `data`, in file order, as many as it holds. A caller that only
    /// plays the file passes none -- the engine reads headers on a 4 kB stack.
    uint8_t  chunkCount = 0;

    /// What the sampler plays: 16-bit samples, mono or stereo, a rate, a frame.
    bool pcm16() const { return bits == 16 && (channels == 1 || channels == 2) && rate > 0 && frames > 0; }

    /// Bytes [offset, offset + bytes) of the file into @p dst; returns how
    /// many it got.
    using ReadAt = uint32_t (*)(void* ctx, uint32_t offset, void* dst, uint32_t bytes);

    /// Walk a file of @p fileBytes. False when it is not a WAVE with a `fmt `
    /// and a `data` chunk the rules above accept.
    bool parse(ReadAt read, void* ctx, uint32_t fileBytes, Chunk* list = nullptr, uint8_t cap = 0);

    /// The same, over stdio from @p f's start.
    bool parse(FILE* f, Chunk* list = nullptr, uint8_t cap = 0);
};

} // namespace crosspad

// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a song loaded from the card (AudioSong::load, not open in the engine)
// sounds like from files that are already rendered: recordings, frozen seq
// tracks, element bakes. It needs no sequencer scene and plays nothing; a seq
// track that is not frozen is reported, not guessed at. The arithmetic of
// SongEngine's flatten() that the two share lives here too.
#pragma once

#include "crosspad/song/AudioSong.hpp"
#include "crosspad/song/ElementResolver.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace crosspad {

/// The fastest tempo a song may have.
constexpr float kMaxSongBpm = 400.0f;
/// Under clips/: a stretched track's blocks, fitted to the song's tempo.
constexpr const char* kStretchDir = ".stretch";

/// Where block @p c fitted to @p outFrames lives: clips/.stretch/, named for
/// its file, the part of it the block plays, the length it is made to and how
/// (an "r" for repitched) -- a tempo, a cut or a mode that changes any of them
/// is another file, so a stale copy is never played.
std::string stretchName(const SongClip& c, uint32_t outFrames, StretchMode mode);

/// The shortest pass a bake can have: a bar of one beat at the fastest tempo a
/// song may have.
uint32_t min_bake_frames(uint32_t sampleRate);

/// The frame block @p i of @p clips ends on at @p spb samples a bar: its own
/// end, the next block's start if that comes first, and @p cut.
uint32_t clip_block_end(const std::vector<SongClip>& clips, size_t i, uint32_t spb, uint32_t cut);

/// An element block from @p start to @p blockEnd going round its bake of
/// @p frames, a pass at a time: the first from the element bar the block starts
/// on (@p from), the rest whole. @p each(start, end, offset) for every pass.
void element_passes(uint32_t from, uint32_t frames, uint32_t spb, uint32_t start, uint32_t blockEnd,
                    const std::function<void(uint32_t start, uint32_t end, uint32_t offset)>& each);

/// A stretch of one file on the song's timeline, in frames of the song.
struct BakedPiece { std::string path; uint32_t start = 0, end = 0, offset = 0; float gain = 1.0f; };

/// What the view could not give, so a caller knows it is not whole.
struct BakedReport {
    std::vector<int>         unfrozenSeq;   ///< seq tracks heard that have no freeze
    std::vector<std::string> missingFiles;  ///< files a block names that are not on the card
    std::vector<std::string> unresolved;    ///< element refs with no bake to play
    bool complete() const { return unfrozenSeq.empty() && missingFiles.empty() && unresolved.empty(); }
};

struct BakedViewEnv {
    std::string songsRoot;
    uint32_t    sampleRate = 0;
    float       bpm = 0;                     ///< the song's (effective) tempo
    std::function<bool(const std::string& path)> exists;
    std::function<const ResolvedElement*(const std::string& ref)> child;   ///< the resolver's answer for an element block
    std::string silentRef;                   ///< blocks of this element contribute nothing (context)
};

/// @p song in [a, b) frames of its own time, from files alone: recordings (their
/// stretched copy where the track stretches and it exists), frozen seq tracks
/// from their freeze files, element blocks from their resolved bakes going
/// round as flatten() has them; silenced tracks left out; gain is the track's
/// volume. Pieces are cut to the window, their offsets moved with them.
std::vector<BakedPiece> baked_view(const AudioSong& song, const BakedViewEnv& env, uint32_t a, uint32_t b,
                                   BakedReport& report);

} // namespace crosspad

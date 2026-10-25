// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a seq track's freeze is made of (SongTrack::freezeKey): the engine
// stamps a freeze with it and plays the freeze only while it is unchanged.
// Free functions, so a song read from the card -- an element the resolver
// looks at, no sequencer behind it -- can be asked the same question.
#pragma once

#include "crosspad/song/AudioSong.hpp"

#include <cstdint>
#include <functional>

namespace crosspad {

/// The sequencer's scenes, for a seq track that plays one: its notes as a
/// pattern and its length in bars (0 is one). Unset where no sequencer holds
/// the song's scenes.
struct SceneLookup {
    std::function<SeqPatternPtr(uint8_t scene)> pattern;
    std::function<uint8_t(uint8_t scene)>       bars;
};

/// The bars @p song plays: AudioSong::playEnd(), and with no length set at
/// least one pass of the scene any seq track not cut into blocks plays.
/// @p unknown is set when that needed a scene @p scenes cannot give.
uint32_t song_end_bars(const AudioSong& song, const SceneLookup& scenes, bool* unknown = nullptr);

/// The bars a seq track's bounce or freeze takes, [@p from, @p to): one cut
/// into blocks from its first block's bar to its last one's end, one playing
/// its scene everywhere over the whole song.
void seq_freeze_range(const AudioSong& song, int track, const SceneLookup& scenes, uint32_t& from, uint32_t& to,
                      bool* unknown = nullptr);

/// The kit or the instrument seq track @p track plays through: a Pitch track
/// the song's instrument, a Kit track its own kit or else the song's.
const std::string& seq_track_sound(const AudioSong& song, int track);

/// What a freeze of seq track @p track at @p sampleRate is made of: its bars,
/// its blocks and their notes (the scene's, for a block without), what it
/// plays through (and SongTrack::freezeSalt, once a sound changed under its
/// name) and the tempo. Never 0, but when it needs a scene @p scenes
/// cannot give: then 0, which no freeze is stamped with.
uint32_t seq_freeze_key(const AudioSong& song, int track, uint32_t sampleRate, const SceneLookup& scenes);

} // namespace crosspad

#pragma once

#include "crosspad/song/AudioSong.hpp"
#include "crosspad/song/ISongEngine.hpp"

namespace crosspad {

/// The seq track Arrange freezes next, the song stopped: one that plays (in use,
/// on an engine's route now -- AudioSong::liveRoute() --, with blocks or its scene everywhere), not the armed one
/// -- the fingers play that -- and not frozen as it is now: never frozen, or stale
/// (its notes, its kit or the tempo changed since). Not one in @p skip (a bit per
/// track), and none while a freeze runs: one at a time. -1 when there is none.
/// @p states is freezeState() of each of the song's kTracks tracks.
int autoFreezeNext(const AudioSong& song, const FreezeState* states, uint32_t skip);

/// Where the two kit slots (AudioSong::kitSlots) go next, playing or stopped,
/// so that ARM moving between kit tracks with kits of their own never waits
/// for the song to stop:
///  - @p want (the track the fingers play: the seq view's, else the armed
///    one) takes a slot at once when it has a kit of its own and holds none:
///    a free one, else one whose track is frozen as it is (it plays its
///    freeze) or plays nothing, else one whose freeze keeps failing
///    (@p skip), else any -- never the one @p prevWant holds while another
///    will do: the track the fingers just left, whose notes play on until
///    the song's ring reaches its freeze, and whose notes changed last.
///  - a track that is not @p want gives its slot back once it plays nothing,
///    or, with the song stopped (@p playing false), once it is frozen as it
///    is: its engine's memory and voices come back. While the song plays a
///    frozen one keeps its kit loaded, idle -- ARM going back to it is then
///    at once, not a load (a live set goes back and forth between two).
///  - a track with a kit of its own that plays, holds none and is not
///    frozen as it is (not one in @p skip) takes a free slot, else one a
///    frozen track only keeps cached: heard live instead of silent, and an
///    engine there for the freeze -- or the harvest -- that ends it.
/// @p states is freezeState() of each of the song's kTracks tracks; @p usable
/// the slots the platform has an engine for (a platform with only the second
/// kit: 1). Writes @p slots; returns whether they differ from the song's.
bool kitSlotsNext(const AudioSong& song, const FreezeState* states, int want, int prevWant, uint32_t skip,
                  bool playing, int8_t (&slots)[AudioSong::kKitSlots], int usable = AudioSong::kKitSlots);

} // namespace crosspad

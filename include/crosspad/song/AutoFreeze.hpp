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

} // namespace crosspad

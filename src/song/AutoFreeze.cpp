// SPDX-License-Identifier: GPL-3.0-or-later

#include "crosspad/song/AutoFreeze.hpp"

namespace crosspad {

int autoFreezeNext(const AudioSong& song, const FreezeState* states, uint32_t skip) {
    for (int t = 0; t < AudioSong::kTracks; ++t)
        if (states[t] == FreezeState::Freezing) return -1;
    for (int t = 0; t < AudioSong::kTracks; ++t) {
        const SongTrack& tr = song.tracks[t];
        if (t == song.armed || (skip & (1u << t))) continue;
        if (!tr.inUse || tr.kind != TrackKind::Seq || song.liveRoute(t) < 0) continue;
        if (song.seqPlaysNothing(t)) continue;           // nothing on it yet, or no more
        if (states[t] != FreezeState::Frozen) return t;
    }
    return -1;
}

} // namespace crosspad

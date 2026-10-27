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

bool kitSlotsNext(const AudioSong& song, const FreezeState* states, int want, int prevWant, uint32_t skip,
                  bool playing, int8_t (&slots)[AudioSong::kKitSlots]) {
    constexpr int kSlots = AudioSong::kKitSlots;
    auto own = [&](int t) { return t >= 0 && t < AudioSong::kTracks && song.seqRoute(t) == 1; };
    auto plays = [&](int t) { return own(t) && !song.seqPlaysNothing(t); };
    auto frozen = [&](int t) { return t >= 0 && states[t] == FreezeState::Frozen; };
    auto skipped = [&](int t) { return t >= 0 && (skip & (1u << t)) != 0; };
    auto slotOf = [&](int t) {
        for (int s = 0; s < kSlots; ++s)
            if (slots[s] == t) return s;
        return -1;
    };

    for (int s = 0; s < kSlots; ++s) {
        slots[s] = song.kitSlots[s];
        if (!own(slots[s])) slots[s] = -1;
    }
    if (!own(want)) want = -1;

    if (want >= 0 && slotOf(want) < 0) {
        int pick = -1;
        auto first = [&](auto pred) {
            for (int s = 0; s < kSlots && pick < 0; ++s)
                if (pred(slots[s])) pick = s;
        };
        /* The track the fingers just left is the last to give its engine
         * up: its notes play on until the song's ring reaches its freeze
         * (SongEngine::pushSeqMap()), and another kit in its engine would
         * play them. */
        first([&](int h) { return h < 0; });
        first([&](int h) { return (frozen(h) || !plays(h)) && h != prevWant; });
        first([&](int h) { return skipped(h) && h != prevWant; });
        first([&](int h) { return h != prevWant; });
        slots[pick < 0 ? kSlots - 1 : pick] = (int8_t)want;
    }

    for (int s = 0; s < kSlots; ++s) {
        const int h = slots[s];
        if (h >= 0 && h != want && ((!playing && frozen(h)) || !plays(h))) slots[s] = -1;
    }

    for (int s = 0; s < kSlots; ++s) {
        if (slots[s] >= 0) continue;
        for (int t = 0; t < AudioSong::kTracks; ++t) {
            if (t == want || !plays(t) || frozen(t) || skipped(t) || slotOf(t) >= 0) continue;
            slots[s] = (int8_t)t;
            break;
        }
    }

    for (int s = 0; s < kSlots; ++s)
        if (slots[s] != song.kitSlots[s]) return true;
    return false;
}

} // namespace crosspad

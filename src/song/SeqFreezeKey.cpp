// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/song/SeqFreezeKey.hpp"

#include <algorithm>

namespace crosspad {

namespace {

/// @p scene's length in bars from @p scenes, one when it cannot say (and @p unknown set).
uint8_t sceneBars(const SceneLookup& scenes, uint8_t scene, bool* unknown) {
    if (!scenes.bars) {
        if (unknown) *unknown = true;
        return 1;
    }
    const uint8_t bars = scenes.bars(scene);
    return bars ? bars : 1;
}

} // namespace

uint32_t song_end_bars(const AudioSong& song, const SceneLookup& scenes, bool* unknown) {
    uint32_t end = song.playEnd();
    if (song.length) return end;
    /* Only a track that plays its scene over everything: one cut into
     * blocks is as long as its blocks, like an audio track. Every track
     * made since blocks had notes is (AudioSong::addSeqTrack()). */
    for (int t = 0; t < AudioSong::kTracks; ++t) {
        if (song.seqRoute(t) < 0 || song.tracks[t].seqBlocks) continue;
        end = std::max<uint32_t>(end, sceneBars(scenes, song.tracks[t].scene, unknown));
    }
    return end;
}

void seq_freeze_range(const AudioSong& song, int track, const SceneLookup& scenes, uint32_t& from, uint32_t& to,
                      bool* unknown) {
    const SongTrack& t = song.tracks[track];
    from = 0;
    to = song_end_bars(song, scenes, unknown);
    if (!t.seqBlocks || t.clips.empty()) return;
    const uint32_t cut = std::max<uint32_t>(to, song.hasRegion() ? song.loopEnd : 0);
    const auto& cs = t.clips;
    from = cs.front().bar;
    to = from;
    for (size_t i = 0; i < cs.size(); ++i) {
        uint32_t end = cs[i].bar + cs[i].bars;
        if (i + 1 < cs.size()) end = std::min(end, cs[i + 1].bar);
        to = std::max(to, std::min(end, cut));
    }
    if (to <= from) { from = 0; to = song_end_bars(song, scenes, unknown); }
}

const std::string& seq_track_sound(const AudioSong& song, int track) {
    const SongTrack& t = song.tracks[track];
    return t.source == SeqSource::Pitch ? song.instrument : t.kit.empty() ? song.kit : t.kit;
}

uint32_t seq_freeze_key(const AudioSong& song, int track, uint32_t sampleRate, const SceneLookup& scenes) {
    bool unknown = false;
    uint32_t h = 2166136261u;
    auto mix = [&h](const void* v, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(v);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
    };
    auto text = [&](const std::string& v) { mix(v.data(), v.size()); mix("", 1); };
    auto notes = [&](const SeqPatternPtr& pt) {
        if (!pt) { mix("", 1); return; }
        mix(&pt->bars, sizeof pt->bars);
        for (const SeqNote& n : pt->notes) {
            mix(&n.tick, 4); mix(&n.len, 4); mix(&n.pad, 1); mix(&n.vel, 1); mix(&n.perf, 1);
        }
    };
    auto scenePattern = [&](uint8_t scene) -> SeqPatternPtr {
        if (scenes.pattern) return scenes.pattern(scene);
        unknown = true;
        return nullptr;
    };
    const SongTrack& t = song.tracks[track];
    uint32_t from = 0, to = 0;
    seq_freeze_range(song, track, scenes, from, to, &unknown);
    mix(&from, 4); mix(&to, 4);
    mix(&song.bpm, sizeof song.bpm);
    mix(&song.beatsPerBar, 1);
    mix(&sampleRate, sizeof sampleRate);
    const uint8_t head[3] = {(uint8_t)t.source, t.scene, (uint8_t)(t.seqBlocks ? 1 : 0)};
    mix(head, sizeof head);
    text(t.source == SeqSource::Pitch ? song.instrument : t.kit.empty() ? song.kit : t.kit);
    /* Left out at 0, so a freeze stamped before the salt moved still reads as it was. */
    if (t.freezeSalt) mix(&t.freezeSalt, sizeof t.freezeSalt);
    SeqPatternPtr scene;
    if (!t.seqBlocks) scene = scenePattern(t.scene);
    for (const auto& c : t.clips) {
        mix(&c.bar, 4); mix(&c.bars, 2); mix(&c.from, 2);
        if (!c.notes && !scene) scene = scenePattern(t.scene);
        notes(c.notes ? c.notes : scene);
    }
    if (!t.seqBlocks) notes(scene);
    if (unknown) return 0;
    return h ? h : 1;
}

} // namespace crosspad

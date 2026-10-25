// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/song/BakedView.hpp"

#include <algorithm>
#include <cstdio>

namespace crosspad {

namespace {

/// A piece cut to [a, b) with its offset moved to match; nothing when it lies outside.
void keep(std::vector<BakedPiece>& out, BakedPiece p, uint32_t a, uint32_t b) {
    const uint32_t start = std::max(p.start, a), end = std::min(p.end, b);
    if (end <= start) return;
    p.offset += start - p.start;
    p.start = start;
    p.end = end;
    out.push_back(std::move(p));
}

template <typename T> void noteOnce(std::vector<T>& list, const T& v) {
    if (std::find(list.begin(), list.end(), v) == list.end()) list.push_back(v);
}

} // namespace

std::string stretchName(const SongClip& c, uint32_t outFrames, StretchMode mode) {
    std::string stem = c.file;
    const size_t dot = stem.rfind('.');
    if (dot != std::string::npos) stem.resize(dot);
    char tail[48];
    std::snprintf(tail, sizeof(tail), "_%u_%u_%u%s.wav", (unsigned)c.offset, (unsigned)c.frames,
                  (unsigned)outFrames, mode == StretchMode::Repitch ? "r" : "");
    return std::string(kStretchDir) + "/" + stem + tail;
}

uint32_t min_bake_frames(uint32_t sampleRate) {
    return (uint32_t)((float)sampleRate * 60.0f / kMaxSongBpm);
}

uint32_t clip_block_end(const std::vector<SongClip>& clips, size_t i, uint32_t spb, uint32_t cut) {
    uint32_t end = (clips[i].bar + clips[i].bars) * spb;
    if (i + 1 < clips.size()) end = std::min(end, clips[i + 1].bar * spb);
    return std::min(end, cut);
}

void element_passes(uint32_t from, uint32_t frames, uint32_t spb, uint32_t start, uint32_t blockEnd,
                    const std::function<void(uint32_t, uint32_t, uint32_t)>& each) {
    uint32_t offset = (uint32_t)((uint64_t)from * spb % frames);
    for (uint32_t at = start; at < blockEnd;) {
        const uint32_t end = std::min(blockEnd, at + (frames - offset));
        each(at, end, offset);
        at = end;
        offset = 0;
    }
}

std::vector<BakedPiece> baked_view(const AudioSong& song, const BakedViewEnv& env, uint32_t a, uint32_t b,
                                   BakedReport& report) {
    std::vector<BakedPiece> out;
    const uint32_t spb = AudioSong::samplesPerBar(env.sampleRate, env.bpm, song.beatsPerBar);
    uint32_t cut = song.playEnd() * spb;
    if (song.hasRegion()) cut = std::max(cut, song.loopEnd * spb);
    const uint32_t minFrames = min_bake_frames(env.sampleRate);
    const std::string bakes = env.songsRoot + "/" + AudioSong::bakeDir() + "/";

    for (int t = 0; t < AudioSong::kTracks; ++t) {
        if (song.silenced(t)) continue;
        const SongTrack& tr = song.tracks[t];
        if (song.seqRoute(t) >= 0) {
            /* A seq track is only what its freeze rendered: its scenes are not here to play. */
            if (tr.freeze.empty()) { report.unfrozenSeq.push_back(t); continue; }
            for (const SongClip& fz : tr.freeze) {
                const std::string path = song.clipPath(fz.file);
                if (!env.exists(path)) { noteOnce(report.missingFiles, path); continue; }
                BakedPiece p;
                p.path = path;
                p.start = fz.bar * spb;
                p.end = std::min((fz.bar + fz.bars) * spb, cut);
                p.offset = fz.offset;
                p.gain = tr.volume;
                keep(out, std::move(p), a, b);
            }
            continue;
        }
        if (tr.kind == TrackKind::Seq) continue;
        for (size_t i = 0; i < tr.clips.size(); ++i) {
            const SongClip& c = tr.clips[i];
            BakedPiece s;
            s.start = c.bar * spb;
            s.end = clip_block_end(tr.clips, i, spb, cut);
            s.gain = tr.volume;
            if (c.isElement()) {
                if (c.element == env.silentRef) continue;
                const ResolvedElement* e = env.child ? env.child(c.element) : nullptr;
                if (!e || !e->exists || e->frames < minFrames) { noteOnce(report.unresolved, c.element); continue; }
                s.path = bakes + e->file;
                element_passes(c.from, e->frames, spb, s.start, s.end, [&](uint32_t from, uint32_t to, uint32_t offset) {
                    BakedPiece pass = s;
                    pass.start = from;
                    pass.end = to;
                    pass.offset = offset;
                    keep(out, std::move(pass), a, b);
                });
                continue;
            }
            /* A stretched track plays its block's fitted copy where it has been made. */
            const std::string fitted = tr.stretch != StretchMode::Off
                ? song.clipPath(stretchName(c, c.bars * spb, tr.stretch)) : std::string();
            if (!fitted.empty() && env.exists(fitted)) {
                s.path = fitted;
            } else {
                s.path = song.clipPath(c.file);
                if (!env.exists(s.path)) { noteOnce(report.missingFiles, s.path); continue; }
                if (c.frames) s.end = std::min(s.end, s.start + c.frames);
                s.offset = c.offset;
            }
            keep(out, std::move(s), a, b);
        }
    }
    return out;
}

} // namespace crosspad

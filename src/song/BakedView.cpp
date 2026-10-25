// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/song/BakedView.hpp"

#include "crosspad/dsp/TimeStretch.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace crosspad {

namespace {

/// A piece cut to [a, b) with its offset moved to match; nothing when it lies outside.
void keep(std::vector<BakedPiece>& out, BakedPiece p, uint32_t a, uint32_t b) {
    const uint32_t start = std::max(p.start, a), end = std::min(p.end, b);
    if (end <= start) return;
    p.offset = (uint32_t)std::min<uint64_t>((uint64_t)p.offset + (start - p.start), UINT32_MAX);
    p.start = start;
    p.end = end;
    out.push_back(std::move(p));
}

template <typename T> void noteOnce(std::vector<T>& list, const T& v) {
    if (std::find(list.begin(), list.end(), v) == list.end()) list.push_back(v);
}

} // namespace

uint32_t bars_to_frames(uint64_t bars, uint32_t spb) {
    return (uint32_t)std::min<uint64_t>(bars * spb, UINT32_MAX);
}

bool stretch_needs_copy(const SongClip& c, uint32_t outFrames) {
    if (!c.frames) return true;
    const double ratio = (double)outFrames / c.frames;
    return std::fabs(ratio - 1.0) >= kStretchSame && ratio >= TimeStretch::kMinRatio && ratio <= TimeStretch::kMaxRatio;
}

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

uint32_t varispeed_frames(uint32_t frames, float bakeBpm, float bpm, uint32_t beat) {
    if (!(bakeBpm > 0.0f) || !(bpm > 0.0f) || !beat || std::fabs(bakeBpm - bpm) <= kSameTempoBpm) return frames;
    /* Counted in beats, not scaled: the bake is whole beats of its tempo, and a
     * pass scaled by the tempi would miss the song's truncated beat a little
     * every beat, drifting against the bars pass after pass. */
    const double beats = std::max(1.0, std::round((double)frames * bakeBpm / ((double)beat * bpm)));
    return (uint32_t)std::min<double>(beats * beat, UINT32_MAX);
}

uint64_t varispeed_step(uint32_t frames, uint32_t pass) {
    if (!pass || pass == frames) return 0;
    return ((uint64_t)frames << kVarispeedFracBits) / pass;
}

uint32_t clip_block_end(const std::vector<SongClip>& clips, size_t i, uint32_t spb, uint32_t cut) {
    uint32_t end = bars_to_frames((uint64_t)clips[i].bar + clips[i].bars, spb);
    if (i + 1 < clips.size()) end = std::min(end, bars_to_frames(clips[i + 1].bar, spb));
    return std::min(end, cut);
}

void element_passes(uint32_t from, uint32_t frames, uint32_t spb, uint32_t start, uint32_t blockEnd,
                    const std::function<void(uint32_t, uint32_t, uint32_t)>& each) {
    if (!frames) return;
    uint32_t offset = (uint32_t)((uint64_t)from * spb % frames);
    for (uint32_t at = start; at < blockEnd;) {
        const uint32_t end = (uint32_t)std::min<uint64_t>(blockEnd, (uint64_t)at + (frames - offset));
        each(at, end, offset);
        at = end;
        offset = 0;
    }
}

std::vector<BakedPiece> baked_view(const AudioSong& song, const BakedViewEnv& env, uint32_t a, uint32_t b,
                                   BakedReport& report) {
    std::vector<BakedPiece> out;
    const uint32_t spb = AudioSong::samplesPerBar(env.sampleRate, env.bpm, song.beatsPerBar);
    uint32_t cut = bars_to_frames(song.playEnd(), spb);
    if (song.hasRegion()) cut = std::max(cut, bars_to_frames(song.loopEnd, spb));
    const uint32_t minFrames = min_bake_frames(env.sampleRate);
    /* A freeze laid on another tempo's grid would be out of time with it. */
    const bool apart = std::fabs((song.bpm > 0.0f ? song.bpm : AudioSong::kDefaultBpm) - env.bpm) > kSameTempoBpm;
    const std::string bakes = env.songsRoot + "/" + AudioSong::bakeDir() + "/";

    for (int t = 0; t < AudioSong::kTracks; ++t) {
        if (song.silenced(t)) continue;
        const SongTrack& tr = song.tracks[t];
        if (song.seqRoute(t) >= 0) {
            /* A seq track is only what its freeze rendered: its scenes are not here to play. */
            if (tr.freeze.empty() || apart) { report.unfrozenSeq.push_back(t); continue; }
            for (const SongClip& fz : tr.freeze) {
                const std::string path = song.clipPath(fz.file);
                if (!env.exists(path)) { noteOnce(report.missingFiles, path); continue; }
                BakedPiece p;
                p.path = path;
                p.start = bars_to_frames(fz.bar, spb);
                p.end = std::min(bars_to_frames((uint64_t)fz.bar + fz.bars, spb), cut);
                p.offset = fz.offset;
                p.gain = tr.volume;
                p.pan = tr.pan;
                keep(out, std::move(p), a, b);
            }
            continue;
        }
        if (tr.kind == TrackKind::Seq) continue;
        for (size_t i = 0; i < tr.clips.size(); ++i) {
            const SongClip& c = tr.clips[i];
            BakedPiece s;
            s.start = bars_to_frames(c.bar, spb);
            s.end = clip_block_end(tr.clips, i, spb, cut);
            s.gain = tr.volume;
            s.pan = tr.pan;
            if (c.isElement()) {
                if (std::find(env.silentRefs.begin(), env.silentRefs.end(), c.element) != env.silentRefs.end()) continue;
                const ResolvedElement* e = env.child ? env.child(c.element) : nullptr;
                if (!e || !e->exists || e->frames < minFrames) { noteOnce(report.unresolved, c.element); continue; }
                s.path = bakes + e->file;
                /* One made at another tempo laid as flatten() lays it: varispeed, a pass the element's bars here. */
                const uint32_t beat = song.beatsPerBar ? spb / song.beatsPerBar : 0;
                const uint32_t pass = varispeed_frames(e->frames, e->bpm, env.bpm, beat);
                s.step = varispeed_step(e->frames, pass);
                element_passes(c.from, pass, spb, s.start, s.end, [&](uint32_t from, uint32_t to, uint32_t offset) {
                    BakedPiece pass = s;
                    pass.start = from;
                    pass.end = to;
                    pass.offset = offset;
                    keep(out, std::move(pass), a, b);
                });
                continue;
            }
            /* A stretched track plays its block's fitted copy where it has been made. */
            const uint32_t fitFrames = bars_to_frames(c.bars, spb);
            const bool needs = tr.stretch != StretchMode::Off && stretch_needs_copy(c, fitFrames);
            const std::string fitted = needs ? song.clipPath(stretchName(c, fitFrames, tr.stretch)) : std::string();
            if (!fitted.empty() && env.exists(fitted)) {
                s.path = fitted;
            } else {
                /* Laid as recorded so something plays, but the report says it is not fitted. */
                if (needs) noteOnce(report.missingFiles, fitted);
                s.path = song.clipPath(c.file);
                if (!env.exists(s.path)) { noteOnce(report.missingFiles, s.path); continue; }
                if (c.frames) s.end = (uint32_t)std::min<uint64_t>(s.end, (uint64_t)s.start + c.frames);
                s.offset = c.offset;
            }
            keep(out, std::move(s), a, b);
        }
    }
    return out;
}

} // namespace crosspad

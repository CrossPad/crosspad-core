// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/song/ElementResolver.hpp"

#include "crosspad/song/BakedView.hpp"

#include <cmath>
#include <utility>

namespace crosspad {

ElementResolver::ElementResolver(std::string songsRoot, uint32_t sampleRate, float bpm, ElementHashReader rd)
    : root_(std::move(songsRoot)), rate_(sampleRate), bpm_(bpm), rd_(std::move(rd)) {
    rd_.childKey = [this](const std::string& ref, uint64_t& key) {
        key = resolve(ref).key;
        return true;
    };
}

const ResolvedElement& ElementResolver::resolve(const std::string& ref) {
    const auto known = done_.find(ref);
    if (known != done_.end()) return known->second;
    if (!open_.insert(ref).second) {
        /* Met again on its own way down: a nest that goes round. Not kept, so the
         * element itself is still resolved when its turn comes back up. */
        cycle_ = ResolvedElement{};
        cycle_.ref = ref;
        cycle_.why = "Element " + ref + " is in a cycle: it contains itself.";
        return cycle_;
    }
    ResolvedElement r = make(ref);
    open_.erase(ref);
    return done_.emplace(ref, std::move(r)).first->second;
}

ResolvedElement ElementResolver::make(const std::string& ref) {
    ResolvedElement r;
    r.ref = ref;
    AudioSong song;
    if (song.load(root_ + "/" + ref) != SongLoad::Loaded) {
        r.why = "Element " + ref + " cannot be read.";
        return r;
    }
    /* A freeze was rendered, and a recording laid, on the grid of the tempo the element had. */
    const bool apart = std::fabs((song.bpm > 0.0f ? song.bpm : AudioSong::kDefaultBpm) - bpm_) > kSameTempoBpm;
    song.bpm = bpm_;
    const uint32_t spb = song.samplesPerBar(rate_);
    r.frames = bars_to_frames(song.playEnd(), spb);

    /* Children first: their keys are part of this one's hash. */
    for (int t = 0; t < AudioSong::kTracks; ++t) {
        if (song.silenced(t)) continue;
        const SongTrack& tr = song.tracks[t];
        if (song.seqRoute(t) >= 0) {
            if (r.why.empty() && tr.freeze.empty()) r.why = "Track " + std::to_string(t + 1) + " is not frozen.";
            if (r.why.empty() && apart) r.why = "Track " + std::to_string(t + 1) + " is frozen at another tempo.";
            for (const SongClip& fz : tr.freeze) {
                uint64_t bytes = 0;
                int64_t modified = 0;
                if (r.why.empty() && !(rd_.info && rd_.info(song.clipPath(fz.file), bytes, modified)))
                    r.why = "Track " + std::to_string(t + 1) + " lost its freeze " + fz.file + ".";
            }
            continue;
        }
        for (const SongClip& c : tr.clips) {
            if (!c.isElement()) {
                if (tr.kind != TrackKind::Audio || !r.why.empty()) continue;
                /* A recording made at another tempo is stretched to the bars when the
                 * element is entered at this one (ElementNav); as recorded it is not
                 * the element at this tempo. */
                if (tr.stretch == StretchMode::Off) {
                    if (apart) r.why = "Track " + std::to_string(t + 1) + " is not fitted to the tempo yet.";
                    continue;
                }
                const uint32_t fit = bars_to_frames(c.bars, spb);
                uint64_t bytes = 0;
                int64_t modified = 0;
                if (stretch_needs_copy(c, fit) &&
                    !(rd_.info && rd_.info(song.clipPath(stretchName(c, fit, tr.stretch)), bytes, modified)))
                    r.why = "Track " + std::to_string(t + 1) + " is not fitted to the tempo yet.";
                continue;
            }
            const ResolvedElement& child = resolve(c.element);
            if (r.why.empty() && !child.exists && !child.bakeable)
                r.why = "Element " + c.element + ": " + (child.why.empty() ? "no bake." : child.why);
        }
    }
    r.bakeable = r.why.empty();
    r.key = element_bake_key(element_content_hash(song, rd_), bpm_, rate_);
    r.file = element_bake_file(r.key);
    uint64_t bytes = 0;
    int64_t modified = 0;
    r.exists = rd_.info && rd_.info(root_ + "/" + AudioSong::bakeDir() + "/" + r.file, bytes, modified) && bytes;
    return r;
}

} // namespace crosspad

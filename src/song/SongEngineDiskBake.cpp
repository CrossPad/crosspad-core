// SPDX-License-Identifier: GPL-3.0-or-later
//
// SongEngine, element blocks against their elements: the relink pass, and bakes
// mixed from files on the card. A song stores the bake each element block plays;
// the element may have changed since -- a recording replaced, a child edited from
// another song -- and the resolver (ElementResolver.hpp) names its bake as it is
// now. A pass links every block whose element's bake is on the card, queues one
// for each element that can be mixed from files already rendered (baked_view,
// BakedView.hpp), and leaves the rest playing the bake they have. The queue runs
// on the worker while the song is stopped and gives way to anything asked of it.
// The state is SongEngine::Impl (SongEngineImpl.hpp); the worker loop is in SongEngine.cpp.

#include "SongEngineImpl.hpp"

namespace crosspad {

namespace {

/// Readers a disk bake holds at once, beside its writer: the card's descriptors
/// are shared with playback and the kit's samples, and a bake keeps to three.
constexpr int kBakeReaders = 2;

float effectiveBpm(const AudioSong& s) { return s.bpm > 0.0f ? s.bpm : AudioSong::kDefaultBpm; }

/// The files a disk bake reads, in kBakeReaders descriptors: one asked for that
/// none holds is opened in place of the one read longest ago.
struct BakeReaders {
    ClipReader slot[kBakeReaders];
    uint32_t   used[kBakeReaders] = {};
    uint32_t   tick = 0;

    ClipReader* get(const std::string& path) {
        int pick = 0;
        for (int i = 0; i < kBakeReaders; ++i) {
            if (slot[i].fd >= 0 && slot[i].path == path) { pick = i; break; }
            if (used[i] < used[pick]) pick = i;
        }
        used[pick] = ++tick;
        if (slot[pick].fd >= 0 && slot[pick].path == path) return &slot[pick];
        return slot[pick].open(path) ? &slot[pick] : nullptr;
    }
};

/// What a baked view lacked, in words for the block's why.
std::string lacking(const BakedReport& r) {
    if (!r.unfrozenSeq.empty()) return "Track " + std::to_string(r.unfrozenSeq.front() + 1) + " is not frozen.";
    if (!r.missingFiles.empty()) return r.missingFiles.front() + " is not on the card.";
    return "Element " + r.unresolved.front() + " has no bake.";
}

} // namespace

std::string SongEngine::Impl::elementSignature(const AudioSong& s) {
    std::string sig = s.dir;
    char num[48];
    std::snprintf(num, sizeof num, "|%.2f", (double)effectiveBpm(s));
    sig += num;
    for (int t = 0; t < kTracks; ++t) {
        for (const SongClip& c : s.tracks[t].clips) {
            if (!c.isElement()) continue;
            std::snprintf(num, sizeof num, "|%d:%u:%u:%u:", t, (unsigned)c.bar, (unsigned)c.bars, (unsigned)c.frames);
            sig += num;
            sig += c.element;
            sig += ':';
            sig += c.file;
        }
    }
    return sig;
}

void SongEngine::Impl::dropBakeQueue() {
    resolver_.reset();
    bakeQueue_.clear();
    bakesMade_.clear();
    bakesFailed_.clear();
    bakeQueued_.store(0);
}

void SongEngine::Impl::forgetElements() {
    dropBakeQueue();
    std::lock_guard<std::mutex> l(elementMutex_);
    elementSeen_.clear();
    bakesOnCard_.clear();
}

/// Worker: see the file comment. Each element is resolved once, at the tempo the
/// song plays it at, children first; the queue it leaves is the open song's
/// blocks only, in the order they come on the tracks. What the last pass found
/// stays readable until this one has its own.
void SongEngine::Impl::doRelink() {
    /* Busy from the request to the last link: a request made meanwhile is one more pass. */
    relinking_.store(true);
    relinkReq_.store(false);
    AudioSong copy;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (opened_) copy = song_;
    }
    dropBakeQueue();
    std::vector<std::string> refs;
    std::set<std::string> files;
    for (const SongTrack& tr : copy.tracks) {
        for (const SongClip& c : tr.clips) {
            if (!c.isElement()) continue;
            if (std::find(refs.begin(), refs.end(), c.element) == refs.end()) refs.push_back(c.element);
            if (!c.file.empty()) files.insert(c.file);
        }
    }
    if (refs.empty()) {
        forgetElements();
        relinking_.store(false);
        return;
    }

    bakeSong_ = copy.dir;
    bakeBpm_ = effectiveBpm(copy);
    resolver_ = std::make_unique<ElementResolver>(p.songsRoot(), sampleRate_, bakeBpm_, cardHashReader());
    std::map<std::string, ElementSeen> seen;
    std::map<std::string, std::pair<std::string, uint32_t>> links;
    for (const std::string& ref : refs) {
        const ResolvedElement& e = resolver_->resolve(ref);
        ElementSeen s;
        s.file = e.file;
        s.frames = e.frames;
        s.exists = e.exists;
        s.why = e.why;
        if (e.exists) links[ref] = {e.file, e.frames};
        else if (e.bakeable) { s.baking = true; bakeQueue_.push_back(ref); }
        seen[ref] = std::move(s);
    }
    std::set<std::string> onCard;
    for (const std::string& f : files)
        if (fsx::isFile(bakePath(f), 1)) onCard.insert(f);
    for (const auto& kv : seen)
        if (kv.second.exists) onCard.insert(kv.second.file);
    {
        std::lock_guard<std::mutex> l(elementMutex_);
        elementSeen_ = std::move(seen);
        bakesOnCard_ = std::move(onCard);
    }
    linkBlocks(bakeSong_, links);
    bakeQueued_.store((uint32_t)bakeQueue_.size());
    relinking_.store(false);
}

void SongEngine::Impl::linkBlocks(const std::string& dir,
                                  const std::map<std::string, std::pair<std::string, uint32_t>>& links) {
    if (links.empty()) return;
    std::lock_guard<std::recursive_mutex> l(songMutex_);
    if (!opened_ || song_.dir != dir) return;
    /* An edit made and not committed yet is a step of its own, as its commit
     * would have made it; the link is none -- nothing the user did. An undo
     * back past it brings the old bake back, and the pass that undo's rebuild
     * asks for links it again. */
    noteChange();
    /* Element blocks moved by an edit not committed yet are a pass of their
     * own still: only a song the last rebuild had seen takes its link's
     * signature as seen. */
    const bool seen = elementSignature(song_) == elementSig_;
    bool changed = false;
    for (SongTrack& tr : song_.tracks) {
        for (SongClip& c : tr.clips) {
            const auto it = c.isElement() ? links.find(c.element) : links.end();
            if (it == links.end() || (c.file == it->second.first && c.frames == it->second.second)) continue;
            c.file = it->second.first;
            c.frames = it->second.second;
            changed = true;
        }
    }
    if (!changed) return;
    pack(song_, last_);
    if (seen) elementSig_ = elementSignature(song_);   // its own link asks for no pass
    rebuild();
    saveAtUs_.store(p.nowUs() + (int64_t)kSaveDelayMs * 1000);
    saveReq_.store(true);
}

const ResolvedElement& SongEngine::Impl::bakeView(const std::string& ref) {
    const auto made = bakesMade_.find(ref);
    return made != bakesMade_.end() ? made->second : resolver_->resolve(ref);
}

void SongEngine::Impl::bakeMade(const std::string& ref, const ResolvedElement& e) {
    ResolvedElement now = e;
    now.exists = true;
    bakesMade_[ref] = now;
    bakeQueue_.erase(std::remove(bakeQueue_.begin(), bakeQueue_.end(), ref), bakeQueue_.end());
    {
        std::lock_guard<std::mutex> l(elementMutex_);
        const auto it = elementSeen_.find(ref);
        if (it != elementSeen_.end()) {
            it->second.exists = true;
            it->second.baking = false;
        }
        bakesOnCard_.insert(now.file);
    }
    linkBlocks(bakeSong_, {{ref, {now.file, now.frames}}});
    bakeQueued_.store((uint32_t)bakeQueue_.size());   // through once its blocks are linked
}

void SongEngine::Impl::bakeFailed(const std::string& ref, const std::string& why) {
    logf(true, "bake %s: %s", ref.c_str(), why.c_str());
    bakesFailed_[ref] = why;
    bakeQueue_.erase(std::remove(bakeQueue_.begin(), bakeQueue_.end(), ref), bakeQueue_.end());
    bakeQueued_.store((uint32_t)bakeQueue_.size());
    std::lock_guard<std::mutex> l(elementMutex_);
    const auto it = elementSeen_.find(ref);
    if (it != elementSeen_.end()) {
        it->second.baking = false;
        it->second.why = why;
    }
}

/// Worker: an element's bake mixed from its baked view at the queue's tempo,
/// into .bake/ as .part and renamed. Its children's bakes go first: one the
/// card does not have yet is put at the head of the queue and made before it.
void SongEngine::Impl::doDiskBake() {
    const std::string ref = bakeQueue_.front();
    const ResolvedElement& e = bakeView(ref);
    if (e.exists) { bakeMade(ref, e); return; }
    AudioSong el;
    if (el.load(std::string(p.songsRoot()) + "/" + ref) != SongLoad::Loaded) {
        bakeFailed(ref, "Element " + ref + " cannot be read.");
        return;
    }
    el.bpm = bakeBpm_;
    /* A bakeable element's children are on the card or bakeable themselves (a
     * nest that goes round is neither), so this ends. */
    for (int t = 0; t < kTracks; ++t) {
        if (el.silenced(t)) continue;
        for (const SongClip& c : el.tracks[t].clips) {
            if (!c.isElement() || bakeView(c.element).exists) continue;
            const auto failed = bakesFailed_.find(c.element);
            if (failed != bakesFailed_.end()) {
                bakeFailed(ref, "Element " + c.element + ": " + failed->second);
                return;
            }
            if (!bakeView(c.element).bakeable) {
                bakeFailed(ref, "Element " + c.element + ": " + bakeView(c.element).why);
                return;
            }
            bakeQueue_.erase(std::remove(bakeQueue_.begin(), bakeQueue_.end(), c.element), bakeQueue_.end());
            bakeQueue_.insert(bakeQueue_.begin(), c.element);
            bakeQueued_.store((uint32_t)bakeQueue_.size());
            return;
        }
    }
    if (e.frames < min_bake_frames(sampleRate_)) {
        bakeFailed(ref, "Element " + ref + " is empty.");
        return;
    }
    BakedViewEnv env;
    env.songsRoot = p.songsRoot();
    env.sampleRate = sampleRate_;
    env.bpm = bakeBpm_;
    env.exists = [](const std::string& path) { return fsx::isFile(path); };
    env.child = [this](const std::string& child) { return &bakeView(child); };
    BakedReport report;
    std::vector<BakedPiece> pieces = baked_view(el, env, 0, e.frames, report);
    if (!report.complete()) {
        bakeFailed(ref, lacking(report));
        return;
    }
    std::string lost;
    switch (mixDown(std::move(pieces), e.frames, bakePath(e.file), lost)) {
        case BakeRun::Done:
            logf(false, "baked %s into %s from the card", ref.c_str(), e.file.c_str());
            bakeMade(ref, e);
            break;
        case BakeRun::GaveWay:
            break;
        case BakeRun::Failed:
            bakeFailed(ref, lost.empty() ? "The bake was not written: the card is full or would not take it."
                                         : lost + " could not be read from the card whole.");
            break;
    }
}

/// Worker: @p pieces summed at their gains into @p total frames at @p path, a
/// chunk at a time, through BakeReaders. Out at once when anything else is
/// asked of the worker -- PLAY among it -- with the .part removed. A source
/// that cannot be opened, or gives fewer frames than it says it holds (cut
/// short, gone since the view was made), fails the bake: silence in its place
/// would be named as the element. @p lost names it.
SongEngine::Impl::BakeRun SongEngine::Impl::mixDown(std::vector<BakedPiece> pieces, uint32_t total,
                                                    const std::string& path, std::string& lost) {
    bakeProgress_.store(0);
    p.yieldIo();   // the streamer first
    if (requestWaiting()) return BakeRun::GaveWay;
    if (!fsx::makeDir(std::string(p.songsRoot()) + "/" + AudioSong::bakeDir())) return BakeRun::Failed;
    std::stable_sort(pieces.begin(), pieces.end(),
                     [](const BakedPiece& a, const BakedPiece& b) { return a.start < b.start; });
    const std::string part = path + ".part";
    WavWriter w;
    if (!w.open(part.c_str(), sampleRate_, 2)) return BakeRun::Failed;
    w.reserve(total);
    BakeRun run = BakeRun::Done;
    {
        BakeReaders readers;
        std::vector<size_t> live;
        size_t first = 0;
        uint32_t chunk = 0;
        for (uint32_t f = 0; f < total && run == BakeRun::Done; ++chunk) {
            const uint32_t n = std::min(sz.chunkFrames, total - f);
            std::memset(acc_, 0, n * 8);
            while (first < pieces.size() && pieces[first].end <= f) ++first;
            live.clear();
            for (size_t i = first; i < pieces.size() && pieces[i].start < f + n; ++i)
                if (pieces[i].end > f) live.push_back(i);
            /* Every other chunk the other way round, so the two files read last
             * are read first again: four tracks at once open two files a chunk,
             * not four. */
            if (chunk & 1) std::reverse(live.begin(), live.end());
            for (const size_t i : live) {
                const BakedPiece& pc = pieces[i];
                ClipReader* r = readers.get(pc.path);
                const uint32_t x = std::max(pc.start, f), y = std::min(pc.end, f + n);
                if (!r || !r->readWhole(x - pc.start + pc.offset, io_, y - x)) {
                    lost = pc.path;
                    run = BakeRun::Failed;
                    break;
                }
                int32_t* d = acc_ + (x - f) * 2;
                for (uint32_t k = 0; k < (y - x) * 2; ++k) d[k] += (int32_t)((float)io_[k] * pc.gain);
            }
            if (run != BakeRun::Done) break;
            for (uint32_t k = 0; k < n * 2; ++k) {
                const int32_t v = acc_[k];
                io_[k] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            }
            if (!w.write(io_, n)) { run = BakeRun::Failed; break; }
            f += n;
            bakeProgress_.store((uint8_t)((uint64_t)f * kBakeMixedPercent / total));
            p.yieldIo();
            if (f < total && requestWaiting()) run = BakeRun::GaveWay;
        }
    }
    const bool closed = w.close();
    if (run == BakeRun::Done) {
        std::remove(path.c_str());   // an empty one left by a cut: FATFS renames over nothing
        if (!closed || !fsx::rename(part, path)) run = BakeRun::Failed;
    }
    if (run != BakeRun::Done) {
        std::remove(part.c_str());
        return run;
    }
    bakeProgress_.store(100);
    return run;
}

ElementClipState SongEngine::Impl::elementClipState(uint8_t track, size_t clip) {
    std::string ref, file;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (track >= kTracks || clip >= song_.tracks[track].clips.size()) return ElementClipState::Fresh;
        const SongClip& c = song_.tracks[track].clips[clip];
        if (!c.isElement()) return ElementClipState::Fresh;
        ref = c.element;
        file = c.file;
    }
    std::lock_guard<std::mutex> l(elementMutex_);
    const auto it = elementSeen_.find(ref);
    if (it == elementSeen_.end()) return ElementClipState::Fresh;
    const ElementSeen& s = it->second;
    if (s.exists && file == s.file) return ElementClipState::Fresh;
    const bool plays = bakesOnCard_.count(file) != 0;
    if (s.baking) return plays ? ElementClipState::Baking : ElementClipState::Missing;
    return plays ? ElementClipState::Stale : ElementClipState::Missing;
}

std::string SongEngine::Impl::elementClipWhy(uint8_t track, size_t clip) {
    std::string ref, file;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (track >= kTracks || clip >= song_.tracks[track].clips.size()) return {};
        const SongClip& c = song_.tracks[track].clips[clip];
        ref = c.element;
        file = c.file;
    }
    std::lock_guard<std::mutex> l(elementMutex_);
    const auto it = elementSeen_.find(ref);
    if (it == elementSeen_.end() || (it->second.exists && file == it->second.file)) return {};
    return it->second.why;
}

} // namespace crosspad

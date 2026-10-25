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
//
// The context stem is a disk bake too: inside an element, the song around it --
// every occurrence of the element silent -- over one pass of it, mixed from the
// card (ContextMix) and played under the element (requestContext()).
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

/// What a context stem's key names the open element's blocks by, wherever they
/// are: they are silent in it, whatever the element holds.
constexpr uint64_t kSilentFocusKey = 0;

/// @p pc heard through a track at @p outer (left, right): the side gains
/// multiplied, given back as a gain and a balance (pan_side_gains()).
void throughTrack(BakedPiece& pc, const float outer[2]) {
    float in[2];
    pan_side_gains(pc.gain, pc.pan, in);
    const float l = in[0] * outer[0], r = in[1] * outer[1];
    const float g = std::max(l, r);
    pc.gain = g;
    pc.pan = g <= 0.0f ? 0.0f : r >= l ? 1.0f - l / g : r / g - 1.0f;
}

/// The song around one element -- the focus -- from the card, worker only:
/// every element whose tree plays the focus is read and mixed from its own
/// sources in place of its bake, the focus's blocks are silent wherever they
/// are, and everything else plays from files already rendered (baked_view),
/// an element whose bake is not on the card yet from the older one its block
/// holds. So every occurrence of the focus is left out, not only those on the
/// way down to it.
class ContextMix {
public:
    ContextMix(std::string songsRoot, uint32_t sampleRate, float bpm, std::string focus, const ElementHashReader& rd)
        : root_(std::move(songsRoot)), rate_(sampleRate), bpm_(bpm), focus_(std::move(focus)), rd_(rd),
          resolver_(root_, rate_, bpm_, rd) {
        rd_.childKey = [this](const std::string& ref, uint64_t& key) {
            key = childKey(ref);
            return true;
        };
    }
    ContextMix(const ContextMix&) = delete;              // rd_.childKey points back at it
    ContextMix& operator=(const ContextMix&) = delete;

    /// @p song's content hash with the focus silent wherever it plays: an
    /// element that holds it by its own such hash, any other by its bake's key.
    uint64_t hash(const AudioSong& song, const std::string& identity) {
        return element_content_hash(song, identity, rd_);
    }

    /// @p song's pieces in [@p a, @p b) of its own time, placed in the stem by
    /// @p shift and heard through @p outer side gains, into @p out. @p identity
    /// is the song's (its project name or ref), @p label names it in what it lacks.
    void pieces(const AudioSong& song, const std::string& identity, const std::string& label, int64_t shift,
                uint32_t a, uint32_t b, const float outer[2], std::vector<BakedPiece>& out) {
        if (b <= a) return;
        BakedViewEnv env;
        env.songsRoot = root_;
        env.sampleRate = rate_;
        env.bpm = bpm_;
        env.exists = [](const std::string& path) { return fsx::isFile(path); };
        env.child = [&](const std::string& ref) { return child(song, identity, label, ref); };
        env.silentRefs.push_back(focus_);
        for (int t = 0; t < AudioSong::kTracks; ++t)
            for (const SongClip& c : song.tracks[t].clips)
                if (c.isElement() && holds(c.element)) env.silentRefs.push_back(c.element);
        BakedReport report;
        for (BakedPiece& pc : baked_view(song, env, a, b, report)) {
            pc.start = (uint32_t)((int64_t)pc.start + shift);
            pc.end = (uint32_t)((int64_t)pc.end + shift);
            throughTrack(pc, outer);
            out.push_back(std::move(pc));
        }
        if (!report.complete()) {
            const std::string why = label + ": " + lacking(report);
            for (int t : report.unfrozenSeq) lack(identity + "|unfrozen " + std::to_string(t), why);
            for (const std::string& f : report.missingFiles) lack(identity + "|missing " + f, why);
            for (const std::string& r : report.unresolved) lack(identity + "|unbaked " + r, why);
        }

        const uint32_t spb = AudioSong::samplesPerBar(rate_, bpm_, song.beatsPerBar);
        uint32_t cut = bars_to_frames(song.playEnd(), spb);
        if (song.hasRegion()) cut = std::max(cut, bars_to_frames(song.loopEnd, spb));
        for (int t = 0; t < AudioSong::kTracks; ++t) {
            const SongTrack& tr = song.tracks[t];
            if (song.silenced(t) || tr.kind != TrackKind::Audio) continue;
            float track[2];
            pan_side_gains(tr.volume, tr.pan, track);
            const float through[2] = {track[0] * outer[0], track[1] * outer[1]};
            for (size_t i = 0; i < tr.clips.size(); ++i) {
                const SongClip& c = tr.clips[i];
                if (!c.isElement() || c.element == focus_ || !holds(c.element)) continue;
                if (std::find(walk_.begin(), walk_.end(), c.element) != walk_.end()) {
                    lack(identity + "|cycle " + c.element, "Element " + c.element + " is in a cycle: it contains itself.");
                    continue;
                }
                const AudioSong* el = load(c.element);
                if (!el) {
                    lack(identity + "|unreadable " + c.element, "Element " + c.element + " cannot be read.");
                    continue;
                }
                /* Laid as baked_view lays a bake: a pass the element's length at
                 * this tempo, the first from the bar the block starts on. */
                const uint32_t frames = bars_to_frames(el->playEnd(), AudioSong::samplesPerBar(rate_, bpm_, el->beatsPerBar));
                if (frames < min_bake_frames(rate_)) continue;
                const std::string inner = el->title.empty() ? "An element" : el->title;
                element_passes(c.from, frames, spb, bars_to_frames(c.bar, spb), clip_block_end(tr.clips, i, spb, cut),
                               [&](uint32_t at, uint32_t to, uint32_t offset) {
                    const uint32_t x = std::max(at, a), y = std::min(to, b);
                    if (y <= x) return;
                    walk_.push_back(c.element);
                    pieces(*el, c.element, inner, shift + at - offset, x - at + offset, y - at + offset, through, out);
                    walk_.pop_back();
                });
            }
        }
    }

    /// Nothing the stem should hold was missing or played from an older bake.
    bool whole() const { return lacking_.empty(); }
    /// Every such thing, one line each, for the stem's key.
    const std::string& lacks() const { return lacking_; }
    /// The first of them, in words.
    const std::string& why() const { return why_; }

private:
    void lack(const std::string& item, const std::string& why) {
        lacking_ += item;
        lacking_ += '\n';
        if (why_.empty()) why_ = why;
    }

    /// What an element block of @p song plays in the stem: its element's bake
    /// as it is now, or -- not on the card yet -- the older bake the block
    /// holds, as playback plays it, so staleness is never silence. With
    /// neither, the resolver's answer, which baked_view reports as unbaked.
    const ResolvedElement* child(const AudioSong& song, const std::string& identity, const std::string& label,
                                 const std::string& ref) {
        const ResolvedElement& now = resolver_.resolve(ref);
        if (now.exists) return &now;
        const std::string at = identity + "|" + ref;
        const auto known = played_.find(at);
        if (known != played_.end()) return known->second;
        const ResolvedElement* plays = &now;
        for (const SongTrack& tr : song.tracks) {
            for (const SongClip& c : tr.clips) {
                if (c.element != ref || c.file.empty() || c.frames < min_bake_frames(rate_) ||
                    !fsx::isFile(root_ + "/" + AudioSong::bakeDir() + "/" + c.file, 1))
                    continue;
                ResolvedElement& old = older_[at];
                old = now;
                old.file = c.file;
                old.frames = c.frames;
                old.exists = true;
                plays = &old;
                lack(identity + "|older " + ref + " " + c.file, label + ": Element " + ref + " plays an older bake.");
                break;
            }
            if (plays != &now) break;
        }
        played_[at] = plays;
        return plays;
    }

    /// @p ref's song as the card has it; null when it cannot be read.
    const AudioSong* load(const std::string& ref) {
        auto it = songs_.find(ref);
        if (it == songs_.end()) {
            auto song = std::make_unique<AudioSong>();
            if (song->load(root_ + "/" + ref) != SongLoad::Loaded) song.reset();
            it = songs_.emplace(ref, std::move(song)).first;
        }
        return it->second.get();
    }

    /// @p ref is the focus or plays it somewhere in its tree, on a track that
    /// is heard. A nest that goes round is not followed.
    bool holds(const std::string& ref) {
        if (ref == focus_) return true;
        const auto known = holds_.find(ref);
        if (known != holds_.end()) return known->second;
        if (!walking_.insert(ref).second) return false;
        bool found = false;
        if (const AudioSong* s = load(ref)) {
            for (int t = 0; t < AudioSong::kTracks && !found; ++t) {
                if (s->silenced(t)) continue;
                for (const SongClip& c : s->tracks[t].clips)
                    if (c.isElement() && holds(c.element)) { found = true; break; }
            }
        }
        walking_.erase(ref);
        holds_[ref] = found;
        return found;
    }

    uint64_t childKey(const std::string& ref) {
        if (ref == focus_) return kSilentFocusKey;
        if (!holds(ref)) return resolver_.resolve(ref).key;
        const auto known = keys_.find(ref);
        if (known != keys_.end()) return known->second;
        /* One met again on its own way down is silent, as pieces() leaves it. */
        const AudioSong* s = load(ref);
        if (!s || !hashing_.insert(ref).second) return kSilentFocusKey;
        const uint64_t key = element_bake_key(element_content_hash(*s, ref, rd_), bpm_, rate_);
        hashing_.erase(ref);
        keys_[ref] = key;
        return key;
    }

    std::string       root_;
    uint32_t          rate_;
    float             bpm_;
    std::string       focus_;
    ElementHashReader rd_;
    ElementResolver   resolver_;
    std::map<std::string, std::unique_ptr<AudioSong>> songs_;   ///< null: not readable
    std::map<std::string, bool> holds_;
    std::set<std::string> walking_;     ///< holds() under way: met again, a nest that goes round
    std::map<std::string, uint64_t> keys_;
    std::set<std::string> hashing_;     ///< childKey() under way, likewise
    std::vector<std::string> walk_;     ///< the elements pieces() is inside
    std::map<std::string, ResolvedElement> older_;           ///< by song|ref: an older bake a block holds
    std::map<std::string, const ResolvedElement*> played_;   ///< by song|ref: what child() gave
    std::string       lacking_;
    std::string       why_;
};

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

/// Worker: @p pieces summed at their gains and pans into @p total frames at @p path, a
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
                float gs[2];
                pan_side_gains(pc.gain, pc.pan, gs);
                int32_t* d = acc_ + (x - f) * 2;
                for (uint32_t k = 0; k < (y - x) * 2; ++k) d[k] += (int32_t)((float)io_[k] * gs[k & 1]);
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

bool SongEngine::Impl::requestContext(const ContextRequest& r) {
    if (!ensureWorker()) return false;
    std::lock_guard<std::recursive_mutex> l(songMutex_);
    if (r.chain.empty()) {
        dropContext(ContextState::None, {});
        return true;
    }
    if (!opened_ || r.chain.back() != song_.name || !valid_name(r.chain.front())) return false;
    for (size_t i = 1; i < r.chain.size(); ++i)
        if (!is_element_ref(r.chain[i])) return false;
    if (!r.frames) {
        dropContext(ContextState::None, "The element is not heard in the song: nothing plays around it.");
        return true;
    }
    dropContext(ContextState::Baking, {});
    ctxReq_ = r;
    ctxSong_ = song_.dir;
    ctxPending_.store(true);
    p.wake();
    return true;
}

std::string SongEngine::Impl::contextWhy() {
    std::lock_guard<std::recursive_mutex> l(songMutex_);
    return ctxWhy_;
}

void SongEngine::Impl::dropContext(ContextState state, const std::string& why) {
    ++ctxGen_;
    ctxPending_.store(false);
    ctxPath_.clear();
    ctxWhy_ = why;
    ctxState_.store(state);
    publishContext();
}

std::vector<Segment> SongEngine::Impl::contextSegments() const {
    if (ctxPath_.empty() || ctxSong_ != song_.dir || std::fabs(effectiveBpm(song_) - ctxBpm_) > kSameTempoBpm) return {};
    Segment s;
    s.path = ctxPath_;
    s.start = ctxAt_;
    s.end = (uint32_t)std::min<uint64_t>((uint64_t)ctxAt_ + ctxFrames_, UINT32_MAX);
    return {s};
}

void SongEngine::Impl::publishContext() {
    std::lock_guard<std::mutex> l(snapMutex_);
    snap_.context = contextSegments();
}

void SongEngine::Impl::contextDone(uint32_t gen, const std::string& path, uint32_t at, uint32_t frames, float bpm,
                                   ContextState state, const std::string& why) {
    std::lock_guard<std::recursive_mutex> l(songMutex_);
    if (gen != ctxGen_) return;
    ctxPath_ = path;
    ctxAt_ = at;
    ctxFrames_ = frames;
    ctxBpm_ = bpm;
    ctxWhy_ = why;
    ctxState_.store(state);
    publishContext();
}

/// Worker: the stem over the request's pass, clamped to the song's end, at the
/// song's tempo -- found in .bake/ by its key, or mixed there as .part and
/// renamed. A song with nothing around the element gets no file. One the card
/// would not take leaves nothing to play, with the why; one that gave way to a
/// request is asked for again.
void SongEngine::Impl::doContext() {
    ContextRequest req;
    uint32_t gen = 0;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (!ctxPending_.exchange(false)) return;
        req = ctxReq_;
        gen = ctxGen_;
    }
    const std::string& name = req.chain.front();
    AudioSong song;
    if (song.load(std::string(p.songsRoot()) + "/" + name) != SongLoad::Loaded) {
        contextDone(gen, {}, 0, 0, 0.0f, ContextState::Incomplete, name + " cannot be read.");
        return;
    }
    const float bpm = effectiveBpm(song);
    const uint32_t spb = AudioSong::samplesPerBar(sampleRate_, bpm, song.beatsPerBar);
    uint32_t cut = bars_to_frames(song.playEnd(), spb);
    if (song.hasRegion()) cut = std::max(cut, bars_to_frames(song.loopEnd, spb));
    const uint32_t from = req.songFrame;
    const uint32_t frames = from < cut ? std::min(req.frames, cut - from) : 0;

    ContextMix mix(p.songsRoot(), sampleRate_, bpm, req.chain.back(), cardHashReader());
    std::vector<BakedPiece> pieces;
    const float unity[2] = {1.0f, 1.0f};
    mix.pieces(song, name, name, -(int64_t)from, from, from + frames, unity, pieces);
    const ContextState done = mix.whole() ? ContextState::Ready : ContextState::Incomplete;
    if (pieces.empty()) {
        contextDone(gen, {}, req.focusFrame, frames, bpm, done, mix.why());
        return;
    }
    const std::string file = context_bake_file(context_bake_key(mix.hash(song, name), from, frames, mix.lacks(), bpm, sampleRate_));
    const std::string path = bakePath(file);
    if (!fsx::isFile(path, 1)) {
        std::string lost;
        switch (mixDown(std::move(pieces), frames, path, lost)) {
            case BakeRun::Done:
                logf(false, "context of %s baked into %s", req.chain.back().c_str(), file.c_str());
                break;
            case BakeRun::GaveWay: {
                std::lock_guard<std::recursive_mutex> l(songMutex_);
                if (gen == ctxGen_) ctxPending_.store(true);
                return;
            }
            case BakeRun::Failed:
                contextDone(gen, {}, req.focusFrame, frames, bpm, ContextState::Incomplete,
                            lost.empty() ? "The song around the element was not written: the card is full or would not take it."
                                         : lost + " could not be read from the card whole.");
                return;
        }
    }
    contextDone(gen, path, req.focusFrame, frames, bpm, done, mix.why());
}

} // namespace crosspad

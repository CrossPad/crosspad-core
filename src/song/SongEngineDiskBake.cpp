// SPDX-License-Identifier: GPL-3.0-or-later
//
// SongEngine, element blocks against their elements: the relink pass, and bakes
// mixed from files on the card. A song stores the bake each element block plays;
// the element may have changed since -- a recording replaced, a child edited from
// another song -- and the resolver (ElementResolver.hpp) names its bake as it is
// now. A pass links every block whose element's bake is on the card, queues one
// for each element that can be mixed from files already rendered (baked_view,
// BakedView.hpp), and leaves the rest playing the bake they have.
//
// All of it is the worker's background work and runs while the song plays too:
// playback's ring comes first (pace(), and a piece of work only with half a ring
// in hand), a mix is made a chunk at a time between top-ups, and a request the
// loop takes comes before it. Its order: the context stem, the relink pass, the
// open song's bakes -- the element just left first, the path up from an edit --
// then, while the player is away (idleBaking()), other songs that play an
// element changed since, relinked in their song.json on the card.
//
// The context stem is a disk bake too: inside an element, the song around it --
// every occurrence of the element silent -- over one pass of it, mixed from the
// card (ContextMix) and played under the element (requestContext()).
// The state is SongEngine::Impl (SongEngineImpl.hpp); the worker loop is in SongEngine.cpp.
//
// The cache these bakes and stems live in is bounded (doEvict()): after the
// first pass and after every bake it is brought under min(kBakeCapBytes,
// kBakeCapShare percent of the card's free room), the least recently used file
// first. A file's last use is a counter in .bake/usage.json, bumped by the relink
// pass and by the context stem. Never removed: a file the open song's tree plays
// (every element block's file, the stem in use), any element's newest bake
// (AudioSong::newestBake, kept with the element) -- an evicted bake is just
// baked again -- and a .part a mix is writing. The parts an earlier run left
// are removed by the first pass.

#include "SongEngineImpl.hpp"

#include <ArduinoJson.h>

namespace crosspad {

namespace {

float effectiveBpm(const AudioSong& s) { return s.bpm > 0.0f ? s.bpm : AudioSong::kDefaultBpm; }

/// How the context mix asks the card whether a file is there (and holds at
/// least so many bytes).
using IsFile = std::function<bool(const std::string& path, uint64_t atLeast)>;
/// Asked before the context mix reads a song.json: false when the work has
/// given way, and nothing more is read.
using GoOn = std::function<bool()>;

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
    ContextMix(std::string songsRoot, uint32_t sampleRate, float bpm, std::string focus, const ElementHashReader& rd,
               IsFile isFile, GoOn goOn)
        : root_(std::move(songsRoot)), rate_(sampleRate), bpm_(bpm), focus_(std::move(focus)), rd_(rd),
          isFile_(std::move(isFile)), goOn_(std::move(goOn)), resolver_(root_, rate_, bpm_, rd) {
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
        env.exists = [this](const std::string& path) { return isFile_(path, 0); };
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
    /// holds, as playback plays it (varispeed, when it was made at another
    /// tempo), so staleness is never silence. With
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
                    !isFile_(root_ + "/" + AudioSong::bakeDir() + "/" + c.file, 1))
                    continue;
                ResolvedElement& old = older_[at];
                old = now;
                old.file = c.file;
                old.frames = c.frames;
                old.bpm = c.bakeBpm;
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
            if (!goOn_() || song->load(root_ + "/" + ref) != SongLoad::Loaded) song.reset();
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
    IsFile            isFile_;
    GoOn              goOn_;
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
    if (mixing(MixJob::For::Element)) endMix(BakeRun::GaveWay);
    resolver_.reset();
    bakeQueue_.clear();
    bakesMade_.clear();
    bakesFailed_.clear();
    bakeQueued_.store(0);
    bakeOther_ = false;
}

void SongEngine::Impl::forgetElements() {
    dropBakeQueue();
    std::lock_guard<std::mutex> l(elementMutex_);
    elementSeen_.clear();
    bakesOnCard_.clear();
}

/// Worker: see the file comment. Each element is resolved once, at the tempo the
/// song plays it at, children first; the queue it leaves is the open song's
/// blocks only, the element just left first, then the others in the order they
/// come on the tracks. What the last pass found stays readable until this one
/// has its own.
void SongEngine::Impl::doRelink() {
    /* Busy from the request to the last link: a request made meanwhile is one more pass. */
    relinking_.store(true);
    relinkReq_.store(false);
    if (!partsSwept_) evictPending_ = true;   // the first pass of this boot
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
    const auto left = std::find(refs.begin(), refs.end(), leftRef_);
    if (left != refs.end()) std::rotate(refs.begin(), left, left + 1);

    bakeSong_ = copy.dir;
    bakeBpm_ = effectiveBpm(copy);
    gaveWay_ = false;
    resolver_ = std::make_unique<ElementResolver>(p.songsRoot(), sampleRate_, bakeBpm_, pacedHashReader());
    std::map<std::string, ElementSeen> seen;
    std::map<std::string, std::pair<std::string, uint32_t>> links;
    /* An element whose blocks here hold another bake than the one it has now
     * changed since they were linked; so may the one just left, whose blocks
     * its leave linked already. Other songs playing either are relinked idle. */
    std::set<std::string> changed;
    if (left != refs.end()) changed.insert(leftRef_);
    for (const std::string& ref : refs) {
        const ResolvedElement& e = resolver_->resolve(ref);
        if (gaveWay_) return relinkGaveWay();
        ElementSeen s;
        s.file = e.file;
        s.frames = e.frames;
        s.exists = e.exists;
        s.why = e.why;
        if (e.exists) {
            links[ref] = {e.file, e.frames};
            pace();
            if (gaveWay_) return relinkGaveWay();
            noteNewestBake(ref, e.file);
        } else if (e.bakeable) { s.baking = true; bakeQueue_.push_back(ref); }
        if (e.exists || e.bakeable) {
            for (const SongTrack& tr : copy.tracks)
                for (const SongClip& c : tr.clips)
                    if (c.element == ref && !c.file.empty() && c.file != e.file) changed.insert(ref);
        }
        seen[ref] = std::move(s);
    }
    std::set<std::string> onCard;
    for (const std::string& f : files) {
        const bool there = pacedIsFile(bakePath(f), 1);
        if (gaveWay_) return relinkGaveWay();
        if (there) onCard.insert(f);
    }
    for (const auto& kv : seen)
        if (kv.second.exists) onCard.insert(kv.second.file);
    for (const std::string& f : onCard) bumpUse(f);
    {
        std::lock_guard<std::mutex> l(elementMutex_);
        elementSeen_ = std::move(seen);
        bakesOnCard_ = std::move(onCard);
    }
    linkBlocks(bakeSong_, links, bakeBpm_);
    bakeQueued_.store((uint32_t)bakeQueue_.size());
    noteChanged(changed);
    relinking_.store(false);
}

void SongEngine::Impl::relinkGaveWay() {
    dropBakeQueue();
    relinkReq_.store(true);
    relinking_.store(false);
}

void SongEngine::Impl::linkBlocks(const std::string& dir,
                                  const std::map<std::string, std::pair<std::string, uint32_t>>& links, float bpm) {
    if (links.empty()) return;
    if (bakeOther_) {
        linkOnCard(dir, links, bpm);
        return;
    }
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
            if (it == links.end() || (c.file == it->second.first && c.frames == it->second.second && c.bakeBpm == bpm))
                continue;
            c.file = it->second.first;
            c.frames = it->second.second;
            c.bakeBpm = bpm;
            changed = true;
        }
    }
    if (!changed) return;
    pack(song_, last_);
    if (seen) elementSig_ = elementSignature(song_);   // its own link asks for no pass
    rebuild();
    /* Written once the song stops, when it plays: see the worker loop. */
    saveAtUs_.store(p.nowUs() + (int64_t)kSaveDelayMs * 1000);
    saveReq_.store(true);
}

void SongEngine::Impl::linkOnCard(const std::string& dir,
                                  const std::map<std::string, std::pair<std::string, uint32_t>>& links, float bpm) {
    AudioSong s;
    if (links.empty() || s.load(dir) != SongLoad::Loaded) return;
    bool changed = false;
    for (SongTrack& tr : s.tracks) {
        for (SongClip& c : tr.clips) {
            const auto it = c.isElement() ? links.find(c.element) : links.end();
            if (it == links.end() || (c.file == it->second.first && c.frames == it->second.second && c.bakeBpm == bpm))
                continue;
            c.file = it->second.first;
            c.frames = it->second.second;
            c.bakeBpm = bpm;
            changed = true;
        }
    }
    if (!changed) return;
    if (s.save()) logf(false, "relinked %s on the card", s.name.c_str());
    else logf(true, "%s: its element blocks could not be relinked on the card", s.name.c_str());
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
    noteNewestBake(ref, now.file);
    bumpUse(now.file);
    if (!bakeOther_) {
        std::lock_guard<std::mutex> l(elementMutex_);
        const auto it = elementSeen_.find(ref);
        if (it != elementSeen_.end()) {
            it->second.exists = true;
            it->second.baking = false;
        }
        bakesOnCard_.insert(now.file);
    }
    linkBlocks(bakeSong_, {{ref, {now.file, now.frames}}}, bakeBpm_);
    if (!bakeOther_) bakeQueued_.store((uint32_t)bakeQueue_.size());   // through once its blocks are linked
}

void SongEngine::Impl::bakeFailed(const std::string& ref, const std::string& why) {
    logf(true, "bake %s: %s", ref.c_str(), why.c_str());
    bakesFailed_[ref] = why;
    bakeQueue_.erase(std::remove(bakeQueue_.begin(), bakeQueue_.end(), ref), bakeQueue_.end());
    if (bakeOther_) return;
    bakeQueued_.store((uint32_t)bakeQueue_.size());
    std::lock_guard<std::mutex> l(elementMutex_);
    const auto it = elementSeen_.find(ref);
    if (it != elementSeen_.end()) {
        it->second.baking = false;
        it->second.why = why;
    }
}

/// Worker: an element's bake mixed from its baked view at the queue's tempo,
/// into .bake/ as .part and renamed -- begun here, made by stepMix(). Its
/// children's bakes go first: one the card does not have yet is put at the
/// head of the queue and made before it. Anything that gave way is looked at
/// again: a resolve cut short may have read the card as empty, so the queue's
/// answers go and the pass that made them runs again.
void SongEngine::Impl::doDiskBake() {
    const std::string ref = bakeQueue_.front();
    gaveWay_ = false;
    auto resolvedAfresh = [this] {
        if (!gaveWay_) return false;
        const bool other = bakeOther_;
        dropBakeQueue();
        if (!other) relinkReq_.store(true);   // another song's is relinked again in its turn
        return true;
    };
    const ResolvedElement e = bakeView(ref);
    if (resolvedAfresh()) return;
    if (e.exists) { bakeMade(ref, e); return; }
    AudioSong el;
    pace();
    if (gaveWay_) return;
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
            if (!c.isElement()) continue;
            const ResolvedElement child = bakeView(c.element);
            if (resolvedAfresh()) return;
            if (child.exists) continue;
            const auto failed = bakesFailed_.find(c.element);
            if (failed != bakesFailed_.end()) {
                bakeFailed(ref, "Element " + c.element + ": " + failed->second);
                return;
            }
            if (!child.bakeable) {
                bakeFailed(ref, "Element " + c.element + ": " + child.why);
                return;
            }
            bakeQueue_.erase(std::remove(bakeQueue_.begin(), bakeQueue_.end(), c.element), bakeQueue_.end());
            bakeQueue_.insert(bakeQueue_.begin(), c.element);
            if (!bakeOther_) bakeQueued_.store((uint32_t)bakeQueue_.size());
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
    env.exists = [this](const std::string& path) { return pacedIsFile(path); };
    env.child = [this](const std::string& child) { return &bakeView(child); };
    BakedReport report;
    std::vector<BakedPiece> pieces = baked_view(el, env, 0, e.frames, report);
    if (resolvedAfresh()) return;
    if (!report.complete()) {
        bakeFailed(ref, lacking(report));
        return;
    }
    auto job = std::make_unique<MixJob>();
    job->what = MixJob::For::Element;
    job->ref = ref;
    job->bake = e;
    startMix(std::move(job), std::move(pieces), e.frames, bakePath(e.file));
}

void SongEngine::Impl::startMix(std::unique_ptr<MixJob> job, std::vector<BakedPiece> pieces, uint32_t total,
                                const std::string& path) {
    bakeProgress_.store(0);
    mix_ = std::move(job);
    MixJob& j = *mix_;
    j.total = total;
    j.path = path;
    std::stable_sort(pieces.begin(), pieces.end(),
                     [](const BakedPiece& a, const BakedPiece& b) { return a.start < b.start; });
    j.pieces = std::move(pieces);
    /* The span's sum from the ring segments' allocator, as a stretch's work:
     * the engine arena first, never the last of the PSRAM the UI lives in, and
     * given back as the mix ends -- a release ends it (mixMustEnd()). */
    mixAcc_ = static_cast<int32_t*>(p.allocRingSegment(sz.chunkFrames * kMixSpanChunks * 8, 1));
    j.noMemory = !mixAcc_;
    if (!mixAcc_ || !fsx::makeDir(std::string(p.songsRoot()) + "/" + AudioSong::bakeDir()) ||
        !j.w.open((path + ".part").c_str(), sampleRate_, 2)) {
        endMix(BakeRun::Failed);
        return;
    }
    /* Room for it found on the card at once, while nothing plays: on a big
     * card that is a scan of the FAT, longer than the ring lasts. */
    if (!streamingNow()) j.w.reserve(total);
}

/// Worker: the streamer first; then a span begun -- its pieces the readers
/// hold first, so a reader is not opened again for them -- or one source read
/// over at most a chunk of it and summed at its gains and pans, or once all
/// are, a chunk of the span written. A source that cannot be opened, or gives
/// fewer frames than it says it holds (cut short, gone since the view was
/// made), fails the bake: silence in its place would be named as the element.
void SongEngine::Impl::stepMix() {
    MixJob& j = *mix_;
    if (j.what == MixJob::For::Context) {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (j.gen != ctxGen_) {   // asked for again, or dropped, since
            endMix(BakeRun::GaveWay);
            return;
        }
    }
    p.yieldIo();
    const uint32_t span = sz.chunkFrames * kMixSpanChunks;
    if (j.spanAt >= j.total) {
        endMix(BakeRun::Done);
        return;
    }
    if (!j.spanLen) {
        j.spanLen = std::min(span, j.total - j.spanAt);
        std::memset(mixAcc_, 0, (size_t)j.spanLen * 8);
        while (j.first < j.pieces.size() && j.pieces[j.first].end <= j.spanAt) ++j.first;
        j.live.clear();
        for (size_t i = j.first; i < j.pieces.size() && j.pieces[i].start < j.spanAt + j.spanLen; ++i)
            if (j.pieces[i].end > j.spanAt) j.live.push_back(i);
        std::stable_partition(j.live.begin(), j.live.end(),
                              [&](size_t i) { return j.readers.holds(j.pieces[i].path); });
        j.next = 0;
        j.written = 0;
        if (!j.live.empty()) j.readAt = std::max(j.pieces[j.live[0]].start, j.spanAt);
    }
    const uint32_t spanEnd = j.spanAt + j.spanLen;
    if (j.next < j.live.size()) {
        const BakedPiece& pc = j.pieces[j.live[j.next]];
        const uint32_t to = std::min(pc.end, spanEnd);
        const uint32_t x = j.readAt, y = std::min(to, x + sz.chunkFrames);
        ClipReader* r = j.readers.get(pc.path);
        if (!r || !readAt(*r, x - pc.start + pc.offset, pc.step, io_, y - x)) {
            j.lost = pc.path;
            endMix(BakeRun::Failed);
            return;
        }
        float gs[2];
        pan_side_gains(pc.gain, pc.pan, gs);
        int32_t* d = mixAcc_ + (size_t)(x - j.spanAt) * 2;
        for (uint32_t k = 0; k < (y - x) * 2; ++k) d[k] += (int32_t)((float)io_[k] * gs[k & 1]);
        j.readAt = y;
        if (y >= to && ++j.next < j.live.size()) j.readAt = std::max(j.pieces[j.live[j.next]].start, j.spanAt);
        return;
    }
    const uint32_t n = std::min(sz.chunkFrames, j.spanLen - j.written);
    const int32_t* a = mixAcc_ + (size_t)j.written * 2;
    for (uint32_t k = 0; k < n * 2; ++k) io_[k] = (int16_t)(a[k] > 32767 ? 32767 : (a[k] < -32768 ? -32768 : a[k]));
    if (!j.w.write(io_, n)) {
        endMix(BakeRun::Failed);
        return;
    }
    j.written += n;
    bakeProgress_.store((uint8_t)((uint64_t)(j.spanAt + j.written) * kBakeMixedPercent / j.total));
    if (j.written < j.spanLen) return;
    j.spanAt = spanEnd;
    j.spanLen = 0;
    if (j.spanAt >= j.total) endMix(BakeRun::Done);
}

void SongEngine::Impl::endMix(BakeRun run) {
    std::unique_ptr<MixJob> job = std::move(mix_);
    MixJob& j = *job;
    if (mixAcc_) p.freeRingSegment(mixAcc_);
    mixAcc_ = nullptr;
    for (ClipReader& r : j.readers.slot) r.close();
    const std::string unwritten = j.noMemory ? "The bake was not made: not enough memory for its mix."
                                : !j.lost.empty() ? j.lost + " could not be read from the card whole."
                                : std::string();
    const std::string part = j.path + ".part";
    const bool closed = j.w.close();
    if (run == BakeRun::Done) {
        std::remove(j.path.c_str());   // an empty one left by a cut: FATFS renames over nothing
        if (!closed || !fsx::rename(part, j.path)) run = BakeRun::Failed;
    }
    if (run != BakeRun::Done) std::remove(part.c_str());
    else bakeProgress_.store(100);
    const std::string file = j.path.substr(j.path.find_last_of('/') + 1);
    if (j.what == MixJob::For::Element) {
        switch (run) {
            case BakeRun::Done:
                logf(false, "baked %s into %s from the card", j.ref.c_str(), file.c_str());
                bakeMade(j.ref, j.bake);
                evictPending_ = true;
                break;
            case BakeRun::GaveWay:
                break;
            case BakeRun::Failed:
                bakeFailed(j.ref, unwritten.empty() ? "The bake was not written: the card is full or would not take it."
                                                    : unwritten);
                break;
        }
        return;
    }
    switch (run) {
        case BakeRun::Done:
            logf(false, "context of %s baked into %s", j.focus.c_str(), file.c_str());
            bumpUse(file);
            evictPending_ = true;
            contextDone(j.gen, j.path, j.at, j.frames, j.bpm, j.state, j.why);
            break;
        case BakeRun::GaveWay: {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            if (j.gen == ctxGen_) ctxPending_.store(true);
            break;
        }
        case BakeRun::Failed:
            contextDone(j.gen, {}, j.at, j.frames, j.bpm, ContextState::Incomplete,
                        unwritten.empty() ? "The song around the element was not written: the card is full or would not take it."
                                          : unwritten);
            break;
    }
}

bool SongEngine::Impl::mixMustEnd() {
    if (mix_->what == MixJob::For::Element && bakeOther_ && !idleNow()) return true;
    return releaseReq_.load() || openReq_.load() || editReq_.load() != 0 || bounceReq_.load() ||
           (mix_->what == MixJob::For::Element && relinkReq_.load());
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
/// renamed (stepMix()). A song with nothing around the element gets no file.
/// One the card would not take leaves nothing to play, with the why; one that
/// gave way to a request is asked for again.
void SongEngine::Impl::doContext() {
    ContextRequest req;
    uint32_t gen = 0;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (!ctxPending_.exchange(false)) return;
        req = ctxReq_;
        gen = ctxGen_;
    }
    gaveWay_ = false;
    auto again = [&] {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (gen == ctxGen_) ctxPending_.store(true);
    };
    const std::string& name = req.chain.front();
    AudioSong song;
    pace();
    if (gaveWay_) return again();
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

    ContextMix mix(p.songsRoot(), sampleRate_, bpm, req.chain.back(), pacedHashReader(),
                   [this](const std::string& path, uint64_t atLeast) { return pacedIsFile(path, atLeast); },
                   [this] {
                       pace();
                       return !gaveWay_;
                   });
    std::vector<BakedPiece> pieces;
    const float unity[2] = {1.0f, 1.0f};
    mix.pieces(song, name, name, -(int64_t)from, from, from + frames, unity, pieces);
    const uint64_t hash = pieces.empty() ? 0 : mix.hash(song, name);
    if (gaveWay_) return again();
    const ContextState done = mix.whole() ? ContextState::Ready : ContextState::Incomplete;
    if (pieces.empty()) {
        contextDone(gen, {}, req.focusFrame, frames, bpm, done, mix.why());
        return;
    }
    const std::string file = context_bake_file(context_bake_key(hash, from, frames, mix.lacks(), bpm, sampleRate_));
    const std::string path = bakePath(file);
    const bool found = pacedIsFile(path, 1);
    if (gaveWay_) return again();
    if (found) {
        bumpUse(file);
        contextDone(gen, path, req.focusFrame, frames, bpm, done, mix.why());
        return;
    }
    auto job = std::make_unique<MixJob>();
    job->what = MixJob::For::Context;
    job->gen = gen;
    job->at = req.focusFrame;
    job->frames = frames;
    job->bpm = bpm;
    job->state = done;
    job->why = mix.why();
    job->focus = req.chain.back();
    startMix(std::move(job), std::move(pieces), frames, path);
}

void SongEngine::Impl::pace() {
    for (;;) {
        const bool streaming = streamingNow();
        if (requestTaken(streaming) || play_.load() == Play::Ended || (idleWork_ && !idleNow())) {
            gaveWay_ = true;
            break;
        }
        if (!streaming || ring_.used() >= ring_.cap / 2) break;
        serviceTake();
        fillRings();
        if (ring_.used() < ring_.cap / 2) p.waitWake(kStreamPollMs);
    }
    p.yieldIo();
}

ElementHashReader SongEngine::Impl::pacedHashReader() {
    ElementHashReader rd = cardHashReader();
    rd.info = [this, info = rd.info](const std::string& path, uint64_t& bytes, int64_t& modified) {
        pace();
        return !gaveWay_ && info(path, bytes, modified);
    };
    rd.read = [this, read = rd.read](const std::string& path, std::string& bytes) {
        pace();
        return !gaveWay_ && read(path, bytes);
    };
    rd.list = [this, list = rd.list](const std::string& dir, const std::function<void(const std::string&)>& each) {
        pace();
        if (!gaveWay_) list(dir, each);
    };
    return rd;
}

bool SongEngine::Impl::pacedIsFile(const std::string& path, uint64_t atLeast) {
    pace();
    return !gaveWay_ && fsx::isFile(path, atLeast);
}

bool SongEngine::Impl::backgroundPending() {
    return mix_ || contextPending() || (relinkReq_.load() && p.storageReady()) ||
           (peaksPending() && p.storageReady()) || (stretchPending() && p.storageReady()) ||
           diskBakePending() || idlePending() || (evictPending_ && p.storageReady()) ||
           (usageDirty_ && !streamingNow() && p.storageReady());
}

void SongEngine::Impl::backgroundStep(bool streaming) {
    /* The song around the element being edited first: it is what the player
     * hears next. An element's mix under way gives way to it. */
    if (contextPending() || mixing(MixJob::For::Context)) {
        if (mixing(MixJob::For::Element)) endMix(BakeRun::GaveWay);
        if (mix_) stepMix();
        else doContext();
        return;
    }
    if (relinkReq_.load() && p.storageReady()) doRelink();
    /* A mix under way goes on before a waveform or a copy is started: its
     * three descriptors are all the background holds. */
    else if (mix_) stepMix();
    else if (!streaming && peaksPending() && p.storageReady()) doPeaks();
    else if (!streaming && stretchPending() && p.storageReady()) doStretch();
    else if (diskBakePending()) doDiskBake();
    else if (evictPending_ && p.storageReady()) doEvict();
    else if (!streaming && usageDirty_ && p.storageReady()) saveUsage();   // counters bumped by relinks, once the song stops
    else if (!streaming && idlePending()) idleStep();
}

namespace {

/// @p name is a bake or a stem of the cache.
bool is_cached_wav(const std::string& name) {
    static const char kExt[] = ".wav";
    constexpr size_t kExtLen = sizeof kExt - 1;
    return name.size() > kExtLen && name.compare(name.size() - kExtLen, kExtLen, kExt) == 0;
}

bool is_part(const std::string& name) {
    const std::string suffix = kPartSuffix;
    return name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string file_of(const std::string& path) { return path.substr(path.find_last_of('/') + 1); }

} // namespace

void SongEngine::Impl::loadUsage() {
    if (usageLoaded_) return;
    usageLoaded_ = true;
    usage_.clear();
    usageClock_ = 0;
    const std::string path = bakePath(kUsageFile);
    std::string text;
    if (!cardHashReader().read(path, text)) return;
    JsonDocument doc;
    /* A file that does not parse, or is not an object, is rebuilt from what is used. */
    if (deserializeJson(doc, text) || !doc.is<JsonObject>()) return;
    for (JsonPair kv : doc.as<JsonObject>()) {
        const uint64_t at = kv.value() | (uint64_t)0;
        usage_[kv.key().c_str()] = at;
        usageClock_ = std::max(usageClock_, at);
    }
}

void SongEngine::Impl::bumpUse(const std::string& file) {
    loadUsage();
    usage_[file] = ++usageClock_;
    usageDirty_ = true;
}

void SongEngine::Impl::saveUsage() {
    if (!usageDirty_) return;
    JsonDocument doc;
    JsonObject o = doc.to<JsonObject>();
    for (const auto& kv : usage_) o[kv.first] = kv.second;
    std::string text;
    serializeJson(doc, text);
    const std::string path = bakePath(kUsageFile), part = path + kPartSuffix;
    FILE* f = std::fopen(part.c_str(), "wb");
    if (!f) return;
    const bool wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = std::fclose(f) == 0;
    std::remove(path.c_str());   // FATFS renames over nothing
    if (wrote && closed && fsx::rename(part, path)) usageDirty_ = false;
    else std::remove(part.c_str());
}

void SongEngine::Impl::noteNewestBake(const std::string& ref, const std::string& file) {
    AudioSong el;
    if (el.load(std::string(p.songsRoot()) + "/" + ref) != SongLoad::Loaded || el.newestBake == file) return;
    el.newestBake = file;
    if (!el.save()) logf(true, "%s: its newest bake could not be recorded", ref.c_str());
}

void SongEngine::Impl::ownBake(const std::string& file) {
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (opened_ && song_.inElementLibrary() && song_.newestBake != file) {
            song_.newestBake = file;
            saveAtUs_.store(p.nowUs() + (int64_t)kSaveDelayMs * 1000);
            saveReq_.store(true);
        }
    }
    bumpUse(file);
    evictPending_ = true;
}

bool SongEngine::Impl::protectedBakes(std::set<std::string>& keep) {
    const std::string root = p.songsRoot();
    std::set<std::string> seen;
    std::vector<std::string> todo;
    auto tree = [&](const AudioSong& s) {
        for (const SongTrack& tr : s.tracks) {
            for (const SongClip& c : tr.clips) {
                if (!c.isElement()) continue;
                if (!c.file.empty()) keep.insert(c.file);
                if (seen.insert(c.element).second) todo.push_back(c.element);
            }
        }
    };
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (opened_) tree(song_);
        if (!ctxPath_.empty()) keep.insert(file_of(ctxPath_));
    }
    /* The open song's tree: what its blocks play, and each element's newest bake. */
    while (!todo.empty()) {
        const std::string ref = todo.back();
        todo.pop_back();
        pace();
        if (gaveWay_) return false;
        AudioSong el;
        const SongLoad got = el.load(root + "/" + ref);
        if (got == SongLoad::Unreadable) return false;   // mid-save, perhaps: what it plays is not known
        if (got != SongLoad::Loaded) continue;
        tree(el);
        if (!el.newestBake.empty()) keep.insert(el.newestBake);
    }
    /* Every other element of the library. */
    std::vector<std::string> library;
    pace();
    if (gaveWay_) return false;
    fsx::listDir(root + "/" + AudioSong::elementsDir(), [&](const std::string& name, bool isDir) {
        if (isDir) library.push_back(std::string(AudioSong::elementsDir()) + "/" + name);
    });
    for (const std::string& ref : library) {
        if (seen.count(ref)) continue;
        pace();
        if (gaveWay_) return false;
        AudioSong el;
        const SongLoad got = el.load(root + "/" + ref);
        if (got == SongLoad::Unreadable) return false;   // its newest bake is not known: nothing is removed
        if (got == SongLoad::Loaded && !el.newestBake.empty()) keep.insert(el.newestBake);
    }
    return true;
}

void SongEngine::Impl::doEvict() {
    evictPending_ = false;
    gaveWay_ = false;
    auto again = [this] { evictPending_ = true; };
    const std::string dir = std::string(p.songsRoot()) + "/" + AudioSong::bakeDir();
    struct Cached { std::string file; uint64_t bytes; int64_t modified; };
    std::vector<Cached> cached;
    std::vector<std::string> parts;
    std::vector<std::string> names;
    pace();
    if (gaveWay_) return again();
    fsx::listDir(dir, [&](const std::string& name, bool isDir) {
        if (!isDir) names.push_back(name);
    });
    /* No mix is under way when a pass runs, so a part found now is one an
     * earlier run left; the first pass of a boot takes them off. */
    uint64_t total = 0;
    for (const std::string& name : names) {
        pace();
        if (gaveWay_) return again();
        if (is_part(name)) {
            if (!partsSwept_) fsx::fs().remove((dir + "/" + name).c_str());
            continue;
        }
        uint64_t bytes = 0;
        int64_t modified = 0;
        if (!is_cached_wav(name) || !fsx::fileInfo(dir + "/" + name, bytes, modified)) continue;
        cached.push_back({name, bytes, modified});
        total += bytes;
    }
    partsSwept_ = true;

    const uint64_t room = p.freeStorageBytes();
    const uint64_t cap = room == UINT64_MAX ? kBakeCapBytes : std::min<uint64_t>(kBakeCapBytes, room / 100 * kBakeCapShare);
    if (total > cap) {
        std::set<std::string> keep;
        if (!protectedBakes(keep)) {
            /* Not knowing what plays, nothing is removed. A pass that gave way is
             * made again; an unreadable element waits for the next bake. */
            if (gaveWay_) again();
            else logf(true, "bake cache: an element could not be read, nothing removed");
            return;
        }
        loadUsage();
        std::stable_sort(cached.begin(), cached.end(), [&](const Cached& a, const Cached& b) {
            const uint64_t ua = usage_.count(a.file) ? usage_[a.file] : 0, ub = usage_.count(b.file) ? usage_[b.file] : 0;
            return ua != ub ? ua < ub : a.modified < b.modified;
        });
        for (const Cached& c : cached) {
            if (total <= cap) break;
            if (keep.count(c.file)) continue;
            pace();
            if (gaveWay_) return again();
            if (!fsx::fs().remove((dir + "/" + c.file).c_str())) continue;
            logf(false, "bake cache: %s removed (%u kB over the cap)", c.file.c_str(), (unsigned)((total - cap) / 1024));
            total -= c.bytes;
            usage_.erase(c.file);
            usageDirty_ = true;
        }
    }
    /* What is no longer on the card is no longer counted. */
    loadUsage();
    std::set<std::string> there;
    for (const Cached& c : cached) there.insert(c.file);
    for (auto it = usage_.begin(); it != usage_.end();) {
        if (there.count(it->first)) { ++it; continue; }
        it = usage_.erase(it);
        usageDirty_ = true;
    }
    if (!streamingNow()) saveUsage();
}

void SongEngine::Impl::idleBaking(bool allowed) {
    if (idleAllowed_.exchange(allowed) != allowed && workerStarted_.load()) p.wake();
}

void SongEngine::Impl::noteChanged(const std::set<std::string>& refs) {
    bool grew = false;
    for (const std::string& r : refs) grew = changed_.insert(r).second || grew;
    if (!grew) return;
    /* The songs to relink are found again, the new ones' too. */
    idleListed_ = false;
    idleScanAsked_ = false;
    idleSongs_.clear();
}

/// Worker, idle: the uses scan is asked for first (the loop makes it as it
/// makes any list), then the songs are taken from it one at a time; the
/// elements they were found for are done with once the last is relinked. A
/// song whose bakes are queued is done when the queue is through.
void SongEngine::Impl::idleStep() {
    if (bakeOther_) {
        dropBakeQueue();
        if (!idleSongs_.empty()) idleSongs_.erase(idleSongs_.begin());
        return;
    }
    if (!idleListed_) {
        if (!idleScanAsked_) {
            idleScanAsked_ = true;
            usesAsked_.fetch_add(1);
            listReq_.fetch_or(kListUses);
            return;
        }
        if (usesPending()) return;
        idleServing_ = changed_;
        idleSongs_ = songsPlaying(idleServing_);
        idleListed_ = true;
        return;
    }
    if (idleSongs_.empty()) {
        for (const std::string& r : idleServing_) changed_.erase(r);
        idleServing_.clear();
        idleListed_ = false;
        idleScanAsked_ = false;
        return;
    }
    if (relinkOther(idleSongs_.front()) && !bakeOther_) idleSongs_.erase(idleSongs_.begin());
}

std::vector<std::string> SongEngine::Impl::songsPlaying(const std::set<std::string>& refs) {
    std::string open;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (opened_) open = song_.name;
    }
    const ElementUses uses = usesIndex();
    std::set<std::string> songs, seen(refs.begin(), refs.end());
    std::vector<std::string> todo(refs.begin(), refs.end());
    while (!todo.empty()) {
        const std::string ref = todo.back();
        todo.pop_back();
        const auto it = uses.find(ref);
        if (it == uses.end()) continue;
        for (const ElementUse& u : it->second) {
            if (u.song) {
                if (u.user != open) songs.insert(u.user);
            } else if (seen.insert(u.user).second) {
                todo.push_back(u.user);
            }
        }
    }
    return {songs.begin(), songs.end()};
}

/// Worker, idle: @p name's song.json read (never opened: the pads keep the
/// kit the player picked), each element block resolved at its tempo; those
/// whose bake is on the card relinked there at once, those that can be mixed
/// from the card queued for this song (bakeOther_) and relinked as each is
/// made. A song that cannot be read, or is the open one now, is passed over.
bool SongEngine::Impl::relinkOther(const std::string& name) {
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (opened_ && song_.name == name) return true;
    }
    gaveWay_ = false;
    idleWork_ = true;
    struct Done { bool& flag; ~Done() { flag = false; } } done{idleWork_};
    pace();
    if (gaveWay_) return false;
    AudioSong s;
    if (s.load(std::string(p.songsRoot()) + "/" + name) != SongLoad::Loaded) return true;
    std::vector<std::string> refs;
    for (const SongTrack& tr : s.tracks)
        for (const SongClip& c : tr.clips)
            if (c.isElement() && std::find(refs.begin(), refs.end(), c.element) == refs.end()) refs.push_back(c.element);
    const float bpm = effectiveBpm(s);
    auto resolver = std::make_unique<ElementResolver>(p.songsRoot(), sampleRate_, bpm, pacedHashReader());
    std::map<std::string, std::pair<std::string, uint32_t>> links;
    std::vector<std::string> queue;
    for (const std::string& ref : refs) {
        const ResolvedElement& e = resolver->resolve(ref);
        if (gaveWay_) return false;
        if (e.exists) links[ref] = {e.file, e.frames};
        else if (e.bakeable) queue.push_back(ref);
    }
    linkOnCard(s.dir, links, bpm);
    if (queue.empty()) return true;
    dropBakeQueue();
    resolver_ = std::move(resolver);
    bakeSong_ = s.dir;
    bakeBpm_ = bpm;
    bakeQueue_ = std::move(queue);
    bakeOther_ = true;
    return true;
}

} // namespace crosspad

// SPDX-License-Identifier: GPL-3.0-or-later
//
// SongEngine, the freeze and bounce: a scene or a seq track rendered to a WAV, and the capture the render feeds it.
// The state is SongEngine::Impl (SongEngineImpl.hpp); the worker loop is in SongEngine.cpp.

#include "SongEngineImpl.hpp"

namespace crosspad {

void SongEngine::Impl::rtCapture(const float* bus, uint32_t frames) {
    RtSection rt(rtGen_);
    /* The seq track's meter: its notes play through the pads, live. */
    float pl = 0.0f, pr = 0.0f;
    for (uint32_t i = 0; i < frames; ++i) {
        pl = std::max(pl, std::fabs(bus[i * 2]));
        pr = std::max(pr, std::fabs(bus[i * 2 + 1]));
    }
    if (pl > padsPeak_[0].load(std::memory_order_relaxed)) padsPeak_[0].store(pl, std::memory_order_relaxed);
    if (pr > padsPeak_[1].load(std::memory_order_relaxed)) padsPeak_[1].store(pr, std::memory_order_relaxed);
    if (!bArmed_.load(std::memory_order_seq_cst)) return;
    if (!bCapturing_) {
        /* Start on the block in which the clock started, so sample 0 of
         * the file is beat one of the scene. */
        MetronomeNode& clk = clk_;
        const uint32_t at = clk.positionSamples();
        if (!clk.running() || clk.countingIn() || at == 0 || at > frames) return;
        bCapturing_ = true;
    }
    /* A freeze's run-in: played, so it rings into what is recorded, and
     * not recorded itself. */
    uint32_t off = 0;
    if (bSkip_) {
        off = std::min(frames, bSkip_);
        bSkip_ -= off;
        if (off == frames) return;
    }
    const uint32_t n = std::min(frames - off, bLeft_);
    if (n * 2 > capture_.size()) { bOverflow_.store(true); return; }
    int16_t* dst = capture_.data();
    bus += off * 2;
    for (uint32_t i = 0; i < n * 2; ++i) {
        float v = bus[i] * 32767.0f;
        dst[i] = (int16_t)(v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
    }
    if (bRing_.push(dst, n) < n) bOverflow_.store(true);
    bLeft_ -= n;
    if (!bLeft_) {
        bCapturing_ = false;
        bArmed_.store(false, std::memory_order_release);
        bDone_.store(true, std::memory_order_release);
    }
}

/// A seq track's bounce takes only the bars its blocks sound in: the
/// take is played in real time, and a one-bar block in a 64-bar song was
/// 64 bars of waiting, nearly all of it silence (20.10.2026). A track
/// that plays its scene over everything is the whole song, as before.
/// Arrange's BOUNCE: a freeze (runFreeze()) whose blocks become the
/// track's audio -- the same renders, found on the card where they are
/// (a track frozen as it is bounces at once), off the clock where not.
bool SongEngine::Impl::bounceSeqTrack(uint8_t track) {
    if (track >= kTracks) return false;
    uint8_t scene = 0;
    uint32_t from = 0, to = 0, key = 0;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        const SongTrack& t = song_.tracks[track];
        if (!t.inUse || t.kind != TrackKind::Seq || song_.liveRoute(track) < 0) return false;
        scene = renderScene(t);
        seqRange(song_, track, from, to);
        key = seqKey(song_, track);
    }
    return requestBounce(scene, track, to - from, from, key, true);
}

/// The scene a render of @p t plays the clock on: its own -- or, when every block
/// holds its notes, so no note of the scene is heard, the first one when the
/// sequencer has no scene of that number (an element made by 2.25.0, which wrote
/// no scenes of its own: each of its freezes failed, "no such scene").
uint8_t SongEngine::Impl::renderScene(const SongTrack& t) const {
    if (t.scene < seq_.sceneCount() || !t.seqBlocks) return t.scene;
    for (const SongClip& c : t.clips)
        if (!c.notes) return t.scene;
    return 0;
}

bool SongEngine::Impl::freezeTrack(uint8_t track) {
    if (track >= kTracks) return false;
    uint8_t scene = 0;
    uint32_t from = 0, to = 0, key = 0;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        const SongTrack& t = song_.tracks[track];
        if (!t.inUse || t.kind != TrackKind::Seq || song_.liveRoute(track) < 0) return false;
        scene = renderScene(t);
        seqRange(song_, track, from, to);
        key = seqKey(song_, track);
    }
    return requestBounce(scene, track, to - from, from, key);
}

FreezeState SongEngine::Impl::freezeState(uint8_t track) {
    if (track >= kTracks) return FreezeState::None;
    if (busy_.load() && bFreeze_ && bTrack_ == track) return FreezeState::Freezing;
    std::lock_guard<std::recursive_mutex> l(songMutex_);
    const SongTrack& t = song_.tracks[track];
    if (t.kind != TrackKind::Seq || t.freeze.empty()) return FreezeState::None;
    return t.freezeKey == seqKey(song_, track) ? FreezeState::Frozen : FreezeState::Stale;
}

/// @p bars 0: one pass of the scene; otherwise that many bars of the
/// song from bar @p fromBar, the scene looping over them, onto @p track
/// on that bar.
/// @p freezeKey set: a freeze (freezeTrack()), the track's engine alone
/// into SongTrack::freeze, stamped with it.
bool SongEngine::Impl::requestBounce(uint8_t scene, uint8_t track, uint32_t bars, uint32_t fromBar,
                                     uint32_t freezeKey, bool toAudio) {
    if (track >= kTracks || !ensureWorker()) return false;
    /* A freeze waits out a moment after one gave way: the PLAY or take that
     * cancelled it is still starting, and a freeze begun in between had it
     * wait a second time (6 s of a frozen screen under REC). Arrange asks
     * again later. */
    if (freezeKey && !toAudio && p.nowUs() < yieldHoldUs_.load()) return false;
    /* A scene's bounce is audio for an audio track; only a seq track's own
     * bounce lands on the seq track. */
    if (!bars) {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (audioTarget(track, false) < 0) return false;
    }
    if (!freezeKey) yieldFreeze();   // a bounce asked for goes before a freeze
    if (busy_.exchange(true)) return false;
    {
        std::lock_guard<std::mutex> l(statusMutex_);
        status_ = BounceStatus{};
        status_.state = BounceState::Running;
    }
    bScene_ = scene;
    bTrack_ = track;
    bBars_ = bars;
    bFrom_ = bars ? fromBar : 0;
    bFreeze_ = bars && freezeKey;
    bFreezeKey_ = freezeKey;
    bToAudio_ = bFreeze_ && toAudio;
    bCancel_.store(false);
    bounceReq_.store(true);
    p.wake();
    return true;
}

BounceStatus SongEngine::Impl::bounceStatus() {
    std::lock_guard<std::mutex> l(statusMutex_);
    return status_;
}

/// A freeze running gives way to anything asked of the worker -- PLAY,
/// a take, an open, an edit, a bounce: cancelled, the track plays live,
/// and this returns once the worker has put the sequencer back (a take
/// or playback started under the freeze's restore lost its song map to
/// it). A bounce does not give way: false.
bool SongEngine::Impl::yieldFreeze() {
    if (!busy_.load()) return true;
    if (!bFreeze_) return false;
    bCancel_.store(true);
    p.wake();
    /* A piece stops at its next 4 kB, then closes its file and removes
     * it: well under this on the board, which only a card stalled in a
     * write runs out. */
    const int64_t t0 = p.nowUs();
    yieldHoldUs_.store(t0 + kYieldHoldUs);
    for (int i = 0; i < 600 && busy_.load(); ++i) p.sleepMs(5);
    dbgMax(dbgYieldMaxUs_, p.nowUs() - t0);
    return !busy_.load();
}

void SongEngine::Impl::seqRange(const AudioSong& song, int track, uint32_t& from, uint32_t& to) const {
    seq_freeze_range(song, track, scenes(), from, to);
}

uint32_t SongEngine::Impl::seqKey(const AudioSong& song, int track) const {
    return seq_freeze_key(song, track, sampleRate_, scenes());
}

/// How seq track @p track is frozen: a block whose notes go round is
/// rendered for two of its passes only -- the first as it starts, the
/// second as every later one sounds, tails of the one before included --
/// and that second pass plays as often as the block goes round. A block
/// no longer than two passes is rendered whole. A block followed by a gap
/// gets what rings on into it. A track with no blocks plays its scene
/// over the song: one block of it.
std::vector<SongEngine::Impl::FreezePiece> SongEngine::Impl::freezePlan(const AudioSong& song, int track, uint32_t spb) const {
    std::vector<FreezePiece> plan;
    const SongTrack& t = song.tracks[track];
    const uint32_t songEnd = endBars(song);
    const uint32_t cut = std::max<uint32_t>(songEnd, song.hasRegion() ? song.loopEnd : 0);
    const uint8_t sb = seq_.sceneBars(t.scene);
    struct Block { uint32_t bar, end, pass, next; };
    std::vector<Block> blocks;
    if (t.seqBlocks && !t.clips.empty()) {
        const auto& cs = t.clips;
        for (size_t i = 0; i < cs.size(); ++i) {
            uint32_t end = cs[i].bar + cs[i].bars;
            const uint32_t next = i + 1 < cs.size() ? cs[i + 1].bar : UINT32_MAX;
            end = std::min(std::min(end, next), cut);
            if (end <= cs[i].bar) continue;
            const uint32_t pass = cs[i].notes ? cs[i].notes->bars : sb;
            blocks.push_back({cs[i].bar, end, pass ? pass : 1u, next});
        }
    } else if (songEnd) {
        blocks.push_back({0, songEnd, sb ? (uint32_t)sb : 1u, UINT32_MAX});
    }
    for (const Block& k : blocks) {
        const uint32_t len = k.end - k.bar;
        const uint32_t gapEnd = std::min(k.next, cut);
        const uint32_t tail = k.end < gapEnd ? std::min(kFreezeTailBars, gapEnd - k.end) : 0;
        const uint32_t pre = std::min(kFreezePreBars, k.bar);
        FreezePiece first;
        first.w0 = k.bar - pre;
        first.pre = pre;
        SongClip c;
        c.bar = k.bar;
        if (len <= 2 * k.pass) {
            first.cap = len + tail;
            c.bars = (uint16_t)(len + tail);
            first.emit.push_back(c);
            plan.push_back(std::move(first));
            continue;
        }
        first.cap = 2 * k.pass;
        c.bars = (uint16_t)k.pass;
        first.emit.push_back(c);
        for (uint32_t at = k.bar + k.pass; at < k.end; at += k.pass) {
            SongClip again;
            again.bar = at;
            again.bars = (uint16_t)std::min(k.pass, k.end - at);
            again.offset = k.pass * spb;
            first.emit.push_back(again);
        }
        plan.push_back(std::move(first));
        if (tail) {
            FreezePiece ring;
            ring.w0 = k.end - 1;
            ring.pre = 1;
            ring.cap = tail;
            SongClip r;
            r.bar = k.end;
            r.bars = (uint16_t)tail;
            ring.emit.push_back(r);
            plan.push_back(std::move(ring));
        }
    }
    return plan;
}

/// Track @p track's notes over the song's samples [w0, w1), moved to
/// start at 0 -- what the sequencer plays for one freeze render.
std::vector<PatternSequencer::SongSpan> SongEngine::Impl::windowSpans(const AudioSong& song, int track, uint32_t spb,
                                                                      uint32_t w0, uint32_t w1) const {
    const SongTrack& t = song.tracks[track];
    const int route = std::max(0, song.seqRoute(track));
    std::vector<PatternSequencer::SongSpan> all;
    if (t.seqBlocks) {
        all = seqSpansOf(t, spb, song.seqTicksPerBar(), w1, (uint8_t)route);
    } else {
        PatternSequencer::SongSpan whole;
        const SeqPatternPtr own = scenePattern(t.scene);
        whole.b = w1;
        whole.route = (uint8_t)route;
        whole.notes = songNotes(own, spb, song.seqTicksPerBar());
        whole.loop = (own->bars ? own->bars : 1) * spb;
        all.push_back(std::move(whole));
    }
    std::vector<PatternSequencer::SongSpan> out;
    for (auto& sp : all) {
        if (sp.b <= w0 || sp.a >= w1) continue;
        if (sp.a < w0) {
            sp.scene += w0 - sp.a;            // where in its notes the window finds it
            sp.a = w0;
        }
        sp.b = std::min(sp.b, w1);
        sp.a -= w0;
        sp.b -= w0;
        out.push_back(std::move(sp));
    }
    return out;
}

/// What one freeze render plays, as a number: the window's notes where
/// they fall in it, the run-in and length, the sound (its name and
/// freezeSalt), the route and the tempo. Two renders with one key are the
/// same audio, so a block the same as another -- or as it was the last
/// time the track froze -- is rendered once and found on the card after.
uint32_t SongEngine::Impl::pieceKey(const AudioSong& song, int track, uint32_t spb, const FreezePiece& pc,
                                    const std::vector<PatternSequencer::SongSpan>& spans) const {
    uint32_t h = 2166136261u;
    auto mix = [&h](const void* v, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(v);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
    };
    auto text = [&](const std::string& v) { mix(v.data(), v.size()); mix("", 1); };
    const SongTrack& t = song.tracks[track];
    const uint32_t head[5] = {pc.pre, pc.cap, spb, (uint32_t)sampleRate_, t.freezeSalt};
    mix(head, sizeof head);
    const uint8_t src[2] = {(uint8_t)t.source, (uint8_t)std::max(0, song.seqRoute(track))};
    mix(src, sizeof src);
    text(t.source == SeqSource::Pitch ? song.instrument : t.kit.empty() ? song.kit : t.kit);
    bool clockScene = false;
    for (const auto& sp : spans) {
        const uint32_t at = sp.loop ? sp.scene % sp.loop : sp.scene;
        const uint32_t w[4] = {sp.a, sp.b, at, sp.loop};
        mix(w, sizeof w);
        mix(&sp.route, 1);
        if (!sp.notes) { clockScene = true; continue; }
        for (const auto& n : *sp.notes) {
            mix(&n.pos, sizeof n.pos); mix(&n.len, sizeof n.len);
            mix(&n.pad, 1); mix(&n.vel, 1); mix(&n.perf, 1);
        }
    }
    if (clockScene) {
        const SeqPatternPtr sc = scenePattern(t.scene);
        for (const SeqNote& n : sc->notes) {
            mix(&n.tick, 4); mix(&n.len, 4); mix(&n.pad, 1); mix(&n.vel, 1); mix(&n.perf, 1);
        }
    }
    return h;
}

/// Worker: freeze track bTrack_ -- every piece of its freezePlan(), each
/// found on the card by its pieceKey() or rendered by runBounce(), then
/// the blocks they give put on the track together, with the key it was
/// asked for. Renders that finished stay when a later one fails or is
/// cancelled: the next freeze finds them and renders only the rest.
void SongEngine::Impl::runFreeze() {
    std::vector<FreezePiece> plan;
    uint32_t spb = 0;
    std::string dir;
    AudioSong where;   // the song's folder, for a sound named inside it
    std::string sound;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        const SongTrack& t = song_.tracks[bTrack_];
        if (!t.inUse || t.kind != TrackKind::Seq) {
            setStatus(BounceState::Failed, "not a seq track");
            return;
        }
        spb = song_.samplesPerBar(sampleRate_);
        plan = freezePlan(song_, bTrack_, spb);
        dir = song_.clipPath(kFreezeDir);
        where.dir = song_.dir;
        sound = seq_track_sound(song_, bTrack_);
    }
    /* What it renders through, read before the render as the card holds it. */
    const uint32_t kit = sound_identity(where, sound, cardHashReader());
    if (plan.empty()) {
        setStatus(BounceState::Failed, "nothing to freeze");
        return;
    }
    fsx::makeDir(dir);
    {
        std::lock_guard<std::mutex> l(statusMutex_);
        status_.pieces = (uint16_t)std::min<size_t>(plan.size(), 0xFFFF);
        status_.piecesDone = 0;
    }
    std::vector<SongClip> clips;
    std::vector<std::string> used;
    uint32_t rendered = 0;
    for (const FreezePiece& pc : plan) {
        /* Asked to give way between two pieces: before the next one's setup,
         * not after it (yieldFreeze() waits for this). */
        if (bCancel_.load()) {
            setStatus(BounceState::Failed, "cancelled");
            return;
        }
        std::string file;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            bWinSpans_ = windowSpans(song_, bTrack_, spb, pc.w0 * spb, (pc.w0 + pc.pre + pc.cap) * spb);
            char name[40];
            std::snprintf(name, sizeof(name), "%s/FRZ%08X.wav", kFreezeDir,
                          (unsigned)pieceKey(song_, bTrack_, spb, pc, bWinSpans_));
            file = name;
        }
        std::string path;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            path = song_.clipPath(file);
        }
        if (!fsx::exists(path)) {
            bWinTotal_ = pc.cap * spb;
            bWinSkip_ = pc.pre * spb;
            bPieceFile_ = file;
            /* A bounce asked for is not given up to a player on its
             * engine: the piece again, once they are quiet. */
            do {
                bPieceOk_ = false;
                bGaveWay_ = false;
                runBounce();
            } while (!bPieceOk_ && bGaveWay_ && bToAudio_ && !bCancel_.load());
            if (!bPieceOk_) {
                if (bGaveWay_) setStatus(BounceState::Failed, bCancel_.load() ? "cancelled" : kGaveWay);
                return;                              // runBounce() said why
            }
            ++rendered;
        }
        used.push_back(file);
        for (SongClip c : pc.emit) {
            c.file = file;
            clips.push_back(std::move(c));
        }
        std::lock_guard<std::mutex> l(statusMutex_);
        ++status_.piecesDone;
    }
    {
        const int64_t tLock = p.nowUs();
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        SongTrack& tr = song_.tracks[bTrack_];
        if (tr.kind != TrackKind::Seq) {
            setStatus(BounceState::Failed, "the track is not a seq track any more");
            return;
        }
        /* What the track played before and plays no longer: one of 2.9's
         * whole-track files, or a render no track names now -- in a freeze,
         * or as a bounced track's audio. */
        auto named = [&](const std::string& f) {
            for (const auto& other : song_.tracks) {
                for (const auto& c : other.freeze)
                    if (c.file == f) return true;
                for (const auto& c : other.clips)
                    if (c.file == f) return true;
            }
            return false;
        };
        std::vector<std::string> old;
        for (const auto& c : tr.freeze) old.push_back(c.file);
        if (bToAudio_) {
            /* A bounce: the renders are the track's audio now, its notes
             * gone with its seq blocks, the slot free for the next idea. */
            song_.bounceSeqTrack(bTrack_);
            for (const auto& c : clips) song_.addClip(bTrack_, c);
        } else {
            tr.freeze = std::move(clips);
            tr.freezeKey = bFreezeKey_;
            tr.freezeKit = kit;
        }
        for (const auto& f : old)
            if (!named(f)) std::remove(song_.clipPath(f).c_str());
        std::vector<std::string> stray;
        fsx::listDir(dir, [&](const std::string& name, bool isDir) {
            if (!isDir && !named(std::string(kFreezeDir) + "/" + name)) stray.push_back(dir + "/" + name);
        });
        for (const auto& f : stray) std::remove(f.c_str());
        logf(false, "%s track %u: %u renders of %u, %u blocks", bToAudio_ ? "bounced" : "froze",
             (unsigned)(bTrack_ + 1), (unsigned)rendered, (unsigned)used.size(),
             (unsigned)(bToAudio_ ? tr.clips.size() : tr.freeze.size()));
        rebuild();
        dbgMax(dbgFzLockMaxUs_, p.nowUs() - tLock);
    }
    doSave();
    setStatus(BounceState::Done, "");
}

std::vector<int> SongEngine::Impl::kitChangedFreezes(const AudioSong& s) {
    std::vector<int> out;
    const ElementHashReader rd = cardHashReader();
    for (int t = 0; t < kTracks; ++t) {
        const SongTrack& tr = s.tracks[t];
        if (tr.kind == TrackKind::Seq && !tr.freeze.empty() && tr.freezeKit &&
            tr.freezeKit != sound_identity(s, seq_track_sound(s, t), rd))
            out.push_back(t);
    }
    return out;
}

void SongEngine::Impl::unfreeze(AudioSong& s, const std::vector<int>& tracks) {
    for (int t : tracks) {
        s.tracks[t].freezeKey = 0;
        ++s.tracks[t].freezeSalt;
    }
}

/// Seq track @p track plays its freeze: one made, not armed, and still
/// what it is.
bool SongEngine::Impl::frozenPlays(const AudioSong& song, int track) const {
    const SongTrack& t = song.tracks[track];
    return t.kind == TrackKind::Seq && !t.freeze.empty() && song.armed != track &&
           t.freezeKey == seqKey(song, track);
}

void SongEngine::Impl::setStatus(BounceState st, const std::string& err) {
    std::lock_guard<std::mutex> l(statusMutex_);
    status_.state = st;
    if (!err.empty()) status_.error = err;
}

void SongEngine::Impl::runBounce() {
    const int64_t tStart = p.nowUs();
    auto fail = [this](const char* why) {
        logf(true, "bounce failed: %s", why);
        setStatus(BounceState::Failed, why);
    };
    PatternSequencer& seq = seq_;
    MetronomeNode& clk = clk_;

    endPlayback();                           // a bounce owns the transport
    if (!opened_) { fail("no card"); return; }
    if (bScene_ >= seq.sceneCount()) { fail("no such scene"); return; }
    if (!p.storageReady()) { fail("card or memory unavailable"); return; }

    /* <kit>_S<scene>_<n>.wav, the first n that is free. */
    std::string kit = "seq";
    if (auto* km = getKitManager()) {
        if (auto* k = km->getCurrentKit()) kit = k->name;
    }
    for (auto& ch : kit) if (!isalnum((unsigned char)ch)) ch = '_';
    /* Short enough that "_S<scene>_<nn>.wav" always fits after it: cut
     * off, the counter went and every bounce after the first reused one
     * name and wrote over that clip. */
    if (kit.size() > kMaxClipPrefix) kit.resize(kMaxClipPrefix);
    std::string file, path;
    if (bFreeze_) {
        /* Its name is what it plays (runFreeze()): written under a
         * working name, so a render cut short is never found as done. */
        file = bPieceFile_;
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        path = song_.clipPath(file) + ".part";
    }
    for (int n = 1; !bFreeze_ && n < 1000; ++n) {
        char buf[80];
        std::snprintf(buf, sizeof(buf), "%s_S%u_%02d.wav", kit.c_str(), (unsigned)(bScene_ + 1), n);
        file = buf;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            path = song_.clipPath(file);
        }
        if (!fsx::exists(path)) break;
    }

    WavWriter w;
    if (!io_) { fail("not enough memory"); return; }
    if (!w.open(path.c_str(), sampleRate_, 2)) { fail("card write failed"); return; }
    /* A sixth of a second of card stall is the least worth recording
     * with; a card that stalls longer than the ring holds fails the take
     * as "card too slow" rather than writing it with holes. */
    if (!bRing_.alloc(sz.bounceRingFrames, kBounceMinFrames)) {
        w.close();
        std::remove(path.c_str());
        char why[64];
        std::snprintf(why, sizeof(why), "not enough memory (%u kB free)",
                      (unsigned)(p.freeMemoryBytes() / 1024));
        fail(why);
        return;
    }

    /* The scene, once, from its bar one: no count-in, and the pads bus at
     * unity (the platform's). All of it is the player's and goes back as
     * it was. */
    const bool countIn = seq.countIn();
    const uint8_t sceneWas = seq.activeScene();
    auto restore = [&] {
        seq.stop();
        seq.clearSongMap();
        seq.setCountIn(countIn);
        seq.selectScene(sceneWas);
        p.endPadsCapture();
    };
    seq.stop();
    seq.setCountIn(false);
    /* The engine the fingers play: the armed seq track's, the pads' kit
     * without one. */
    bool fingersOnIt = false;
    if (bFreeze_) {
        int route = 0;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            route = std::max(0, song_.seqRoute(bTrack_));
            fingersOnIt = std::max(0, song_.liveRoute(song_.armedSeqTrack())) == route;
        }
        /* A bounce asked for waits for the player before it takes their
         * engine off the outputs (a freeze is refused below instead). */
        while (bToAudio_ && fingersOnIt && p.padsPlayedRecently() && !bCancel_.load()) p.sleepMs(20);
        p.beginRouteCapture((uint8_t)route);
    } else {
        p.beginPadsCapture();
    }
    seq.selectScene(bScene_);

    /* One pass of the scene, or -- a seq track's bounce -- the scene
     * looping over the song's bars, which is what was heard. */
    const uint32_t bars = bBars_ ? bBars_ : (seq.sceneBars(bScene_) ? seq.sceneBars(bScene_) : 1);
    const uint32_t total = bFreeze_ ? bWinTotal_ : clk.samplesPerBeat() * clk.beatsPerBar() * bars;
    if (!w.reserve(total)) {
        restore();
        bRing_.release();
        w.close();
        std::remove(path.c_str());
        fail("card full");
        return;
    }
    bBpm_ = seq.getClickBpm();
    /* A seq track cut into blocks is bounced as it plays: the scene only
     * under them, on the clock's bars from the song's bar bFrom_ -- the
     * clock's sample 0. */
    if (bFreeze_) {
        seq.setSongMap(bWinSpans_, 0, 0);       // runFreeze()'s window, run-in first
    } else if (bBars_) {
        std::vector<PatternSequencer::SongSpan> spans;
        bool mapped = false;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            const SongTrack& tr = song_.tracks[bTrack_];
            /* A track off the pads' route never plays the clock's scene:
             * its notes are mapped whether it has blocks or not. */
            if (tr.kind == TrackKind::Seq && (tr.seqBlocks || song_.seqRoute(bTrack_) > 0)) {
                const int route = song_.seqRoute(bTrack_);
                const uint32_t spb = clk.samplesPerBeat() * clk.beatsPerBar();
                const uint32_t off = bFrom_ * spb;
                std::vector<PatternSequencer::SongSpan> all;
                if (tr.seqBlocks) {
                    all = seqSpansOf(tr, spb, song_.seqTicksPerBar(), off + total,
                                     (uint8_t)(route > 0 ? route : 0));
                } else {
                    /* Its scene over the whole song, as flatten() lays it. */
                    PatternSequencer::SongSpan whole;
                    const SeqPatternPtr own = scenePattern(tr.scene);
                    whole.b = off + total;
                    whole.route = (uint8_t)route;
                    whole.notes = songNotes(own, spb, song_.seqTicksPerBar());
                    whole.loop = (own->bars ? own->bars : 1) * spb;
                    all.push_back(std::move(whole));
                }
                /* bFrom_ is the first block's bar: none starts before it. */
                for (auto& sp : all) {
                    if (sp.a < off) continue;
                    sp.a -= off;
                    sp.b -= off;
                    spans.push_back(std::move(sp));
                }
                mapped = true;
            }
        }
        if (mapped) seq.setSongMap(spans, 0, 0);
    }
    {
        std::lock_guard<std::mutex> l(statusMutex_);
        status_.framesTotal = total;
        status_.file = file;
    }
    bLeft_ = total;
    bSkip_ = bFreeze_ ? bWinSkip_ : 0;
    bCapturing_ = false;
    bOverflow_.store(false);
    bDone_.store(false);
    bArmed_.store(true, std::memory_order_seq_cst);
    /* A bounce or a freeze is a render to a file, not to the speakers:
     * off the clock, as fast as the card lets it, where the platform can
     * (ISongEnginePlatform::beginOffline()); in real time where not. */
    const bool offline = p.beginOffline();

    int16_t* chunk = io_;
    const char* error = nullptr;
    /* A freeze is never heard: off the clock the outputs are silent, and in
     * real time its engine is kept off them (beginRouteCapture()) -- which
     * would silence a player on that engine and put their notes in the take.
     * So with someone playing it the freeze waits for quiet: refused here,
     * or given up at a press mid-take, and tried again later. */
    const bool waitQuiet = bFreeze_ && fingersOnIt;
    if (waitQuiet && p.padsPlayedRecently()) error = kGaveWay;
    else seq.playback();

    const int64_t deadline = p.nowUs() +
                             (int64_t)(total + bSkip_) * 1000000 / sampleRate_ + 3000000;
    char why[80];
    uint32_t done = 0;
    int64_t maxGapUs = 0, lastPop = 0;   // from the first block on: not the wait for bar one
    int64_t firstPop = 0;
    uint32_t ioUs = 0, ioMaxUs = 0, ioChunks = 0, drain = 0;
    bool tooSlow = false;
    bool seqStopped = false;
    while (!error) {
        if (waitQuiet && p.padsPlayedRecently()) { error = kGaveWay; break; }
        /* A lost block is a failed take, whatever else is true: checked
         * first, so a ring that overflowed and then drained is never
         * reported as a finished clip. */
        if (bOverflow_.load()) { tooSlow = true; break; }
        /* Before the next piece, not only once the ring runs dry: while
         * it records there is always a piece, and PLAY or REC waiting on
         * a freeze to let go waited the take out (yieldFreeze()). */
        if (bCancel_.load()) { error = "cancelled"; break; }
        /* The take is complete the moment RT has its last frame: stop the
         * scene there, not when the card has caught up. */
        if (!seqStopped && bDone_.load()) { seq.stop(); seqStopped = true; }
        /* Only whole pieces, each ending on a 4 kB boundary of the file
         * (the first fills up the header's), until the take's last: a
         * piece ending mid-sector made the filesystem read that sector
         * back before writing it, and a short piece is a card command
         * for less than it costs. */
        const uint32_t want = done ? sz.chunkFrames : firstChunkFrames();
        const uint32_t n = bRing_.used() >= want || bDone_.load() ? bRing_.pop(chunk, want) : 0;
        if (n) {
            const int64_t t0 = p.nowUs();
            if (!firstPop) {
                firstPop = t0;
                dbgMax(dbgFzSetupMaxUs_, t0 - tStart);
            }
            if (!p.writeIo(w.stream(), chunk, n * 4)) { error = "card write failed"; break; }
            w.advance(n);
            const int64_t now = p.nowUs();
            const uint32_t us = (uint32_t)(now - t0);
            ioUs += us;
            if (us > ioMaxUs) ioMaxUs = us;
            ++ioChunks;
            if (lastPop) maxGapUs = std::max(maxGapUs, now - lastPop);
            lastPop = now;
            done += n;
            /* How the card keeps up: bytes per millisecond from the start
             * of the first piece is kB/s. */
            const uint32_t ms = (uint32_t)(now - firstPop) / 1000;
            drain = ms ? done * 4 / ms : 0;
            {
                std::lock_guard<std::mutex> l(statusMutex_);
                status_.framesDone = done;
                status_.chunks     = ioChunks;
                status_.chunkAvgUs = ioUs / ioChunks;
                status_.chunkMaxUs = ioMaxUs;
                status_.drainKBps  = drain;
            }
            p.yieldIo();
            continue;
        }
        if (bDone_.load() && !bRing_.used()) break;
        if (bCancel_.load()) { error = "cancelled"; break; }
        if (p.nowUs() > deadline) { error = "timed out"; break; }
        p.sleepMs(2);
    }
    if (tooSlow) {
        std::snprintf(why, sizeof(why), "card too slow (stalled %u ms, buffer %u ms, drain %u kB/s)",
                      (unsigned)(maxGapUs / 1000),
                      (unsigned)((uint64_t)bRing_.cap * 1000 / sampleRate_), (unsigned)drain);
        error = why;
    }
    if (!error && done != total) error = "take incomplete";

    const int64_t tTail = p.nowUs();
    bArmed_.store(false, std::memory_order_seq_cst);
    restore();
    if (offline) p.endOffline();
    waitRtOut();
    bRing_.release();
    const bool closed = w.close();

    if (error || !closed) {
        std::remove(path.c_str());
        dbgMax(dbgFzTailMaxUs_, p.nowUs() - tTail);
        bGaveWay_ = error && std::strcmp(error, kGaveWay) == 0;
        if (!bGaveWay_) fail(error ? error : "card write failed");   // runFreeze() decides
        return;
    }
    logf(false, "bounced scene %u to %s (%u frames)", (unsigned)(bScene_ + 1), file.c_str(), (unsigned)done);
    if (bFreeze_) {                              // one piece of runFreeze()'s
        std::string done;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            done = song_.clipPath(file);
        }
        std::remove(done.c_str());
        if (!fsx::rename(path, done)) {
            std::remove(path.c_str());
            fail("card write failed");
            return;
        }
        bPieceOk_ = true;
        return;
    }
    /* Onto its track, after the last block. An empty song takes the tempo
     * of the clip it is given -- its grid is only ever as right as its
     * first clip, and a song whose blocks were all deleted is a new start,
     * not a tempo to keep. */
    std::string note;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (song_.bpm <= 0.0f || song_.songEnd() == 0) song_.bpm = bBpm_;
        /* Its length on the song's grid, not the scene's bar count: a
         * scene at another tempo is a different number of the song's
         * bars, and a block sized by the scene cut the end of the take
         * off (95 BPM in a 120 BPM song lost a fifth of it). */
        const uint32_t spBar = song_.samplesPerBar(sampleRate_);
        const int target = bBars_ ? (int)bTrack_ : audioTarget(bTrack_, true);
        if (target < 0) {
            std::remove(path.c_str());
            fail("no audio track to put it on");
            return;
        }
        bTrack_ = (uint8_t)target;
        SongClip c;
        c.file = file;
        /* A seq track's bounce sits where its first block did; a
         * scene's goes after the track's last block. */
        c.bar  = bBars_ ? bFrom_ : song_.trackEnd(bTrack_);
        c.bars = std::max<uint32_t>(1, (done + spBar - 1) / spBar);
        song_.addClip(bTrack_, c);
        /* A seq track's bounce: its notes are this clip now, and the slot
         * is free for the next idea. */
        if (bBars_) song_.bounceSeqTrack(bTrack_);
        rebuild();
        if (std::fabs(song_.bpm - bBpm_) >= 0.5f) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "scene at %d BPM, song at %d BPM: off the bar grid",
                          (int)lroundf(bBpm_), (int)lroundf(song_.bpm));
            note = buf;
        }
    }
    doSave();
    setStatus(BounceState::Done, note);
}

} // namespace crosspad

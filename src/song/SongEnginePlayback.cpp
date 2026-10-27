// SPDX-License-Identifier: GPL-3.0-or-later
//
// SongEngine, the playback: the render (mix), the rings the worker fills, flattening the song, stretch, the live launcher.
// The state is SongEngine::Impl (SongEngineImpl.hpp); the worker loop is in SongEngine.cpp.

#include "SongEngineImpl.hpp"

namespace crosspad {

void SongEngine::Impl::mix(float* out, uint32_t frames) {
    RtSection rt(rtGen_);
    Play st = play_.load(std::memory_order_seq_cst);
    uint32_t skip = 0;             // frames of this block before the song's first
    if (st == Play::Counting) {
        skip = countIn(out, frames);
        if (skip == frames) return;
    } else if (st == Play::Armed) {
        /* The clock is mixed before this node, so the block in which its
         * position first moves off where it was started is the block it
         * started in, and how far it moved says where in the block: a
         * count-in hands over mid-block, and the song starts on its sample. */
        MetronomeNode& clk = clk_;
        const uint32_t ap = armPos_.load(std::memory_order_relaxed);
        const uint32_t cp = clk.positionSamples();
        if (!clk.running() || clk.countingIn() || cp == ap) return;
        const uint32_t loop = clk.loopSamples();
        const uint32_t moved = loop ? (cp + loop - ap) % loop : cp - ap;
        skip = moved < frames ? frames - moved : 0;
    }
    if (st == Play::Counting || st == Play::Armed) {
        /* Only from where it was: a stop the worker published since this
         * block began must not be turned back into Running. */
        Play expected = st;
        if (!play_.compare_exchange_strong(expected, Play::Running, std::memory_order_seq_cst)) return;
        st = Play::Running;
    }
    if (st != Play::Running) return;
    if (frames * 2 > scratch_.size()) return;
    const uint32_t n = frames - skip;
    played_.fetch_add(n, std::memory_order_release);
    const uint32_t got = ring_.pop(scratch_.data(), n);
    {
        /* How close the card came to falling behind, for ASONG_DBG. */
        const uint32_t left = ring_.used();
        if (left < dbgRingMin_.load(std::memory_order_relaxed)) dbgRingMin_.store(left, std::memory_order_relaxed);
    }
    if (got < n) {
        /* The card fell behind. Play silence and tell the worker how far
         * to skip, so the song comes back in time, not late. */
        debt_.fetch_add(n - got, std::memory_order_relaxed);
        underruns_.fetch_add(1, std::memory_order_relaxed);
        underAt_[workerAt_.load(std::memory_order_relaxed)].fetch_add(1, std::memory_order_relaxed);
    }
    const int16_t* s = scratch_.data();
    float* o = out + skip * 2;
    for (uint32_t i = 0; i < got * 2; ++i) o[i] += (float)s[i] * (1.0f / 32768.0f);
    uint32_t pos = pos_.load(std::memory_order_relaxed) + n;
    const uint32_t end = end_.load(std::memory_order_relaxed);
    const bool loop = loop_.load(std::memory_order_relaxed);
    const uint32_t la = loopA_.load(std::memory_order_relaxed), lb = loopB_.load(std::memory_order_relaxed);
    const uint32_t lim = loop && lb > la ? lb : end;
    if (lim && pos >= lim) {
        /* Modulo, not minus: the song can be shortened while it plays. */
        if (loop) pos = wrap_position(pos, end, la, lb);
        else {
            Play expected = Play::Running;
            play_.compare_exchange_strong(expected, Play::Ended, std::memory_order_seq_cst);
        }
    }
    pos_.store(pos, std::memory_order_relaxed);
}

/// RT: the song's own count-in, the metronome's click (see MetronomeNode)
/// on every beat, a fifth up on the downbeat. @return the frames of the
/// block it took -- all of them until the last beat is out.
uint32_t SongEngine::Impl::countIn(float* out, uint32_t frames) {
    const uint32_t spb = cinBeat_ ? cinBeat_ : 1;
    uint32_t left = cinLeft_;
    uint32_t i = 0;
    for (; i < frames && left; ++i, --left) {
        const uint32_t at = cinTotal_ - left;
        if (at % spb == 0) {
            clickLeft_ = clickLen_;
            const bool downbeat = (at / spb) % cinBpb_ == 0;
            step_ = (downbeat ? 1500.0f : 1000.0f) * 6.2831853f / (float)sampleRate_;
            phase_ = 0.0f;
        }
        if (clickLeft_) {
            const float env = (float)clickLeft_ / (float)clickLen_;
            const float v = std::sin(phase_) * env * env * kClickLevel;
            out[i * 2] += v;
            out[i * 2 + 1] += v;
            phase_ += step_;
            --clickLeft_;
        }
    }
    cinLeft_ = left;
    cinBeatsLeft_.store((uint8_t)((left + spb - 1) / spb), std::memory_order_relaxed);
    return i;
}

/// Track @p t's level now, 0..1 a side: an audio track's loudest frame
/// in the last few hops RT has played (its gain applied), the seq
/// track's the pads' since the last ask. 0 while stopped.
void SongEngine::Impl::trackLevels(int t, float& left, float& right) {
    left = right = 0.0f;
    if (t < 0 || t >= kTracks) return;
    /* A seq track's are its engine's, where the platform can tell the
     * engines apart; else the pads', which only the pads' route plays. */
    const int8_t route = seqRoute_[t].load(std::memory_order_relaxed);
    const bool seq = route >= 0;
    float pl = 0.0f, pr = 0.0f;
    if (seq && !p.seqLevel((uint8_t)route, pl, pr) && route == 0) {
        pl = padsPeak_[0].exchange(0.0f, std::memory_order_relaxed);
        pr = padsPeak_[1].exchange(0.0f, std::memory_order_relaxed);
    }
    if (play_.load(std::memory_order_relaxed) != Play::Running) return;
    if (seq) {
        left = std::min(1.0f, pl);
        right = std::min(1.0f, pr);
        return;
    }
    const uint32_t hop = ring_.tail.load(std::memory_order_acquire) / kMeterHop;
    uint32_t m[2] = {0, 0};
    for (uint32_t back = 0; back < kMeterRead && back <= hop; ++back) {
        const Meter& s = meter_[(hop - back) % kMeterSlots];
        if (s.hop.load(std::memory_order_acquire) != hop - back) continue;
        for (int c = 0; c < 2; ++c) m[c] = std::max<uint32_t>(m[c], s.peak[t][c].load(std::memory_order_relaxed));
    }
    left = (float)m[0] / 32767.0f;
    right = (float)m[1] / 32767.0f;
}

uint32_t SongEngine::Impl::framesPerBar() {
    std::lock_guard<std::recursive_mutex> l(songMutex_);
    return song_.samplesPerBar(sampleRate_);
}

// ── Bounce ────────────────────────────────────────────────────────────
uint32_t SongEngine::Impl::playEndBars() {
    std::lock_guard<std::recursive_mutex> l(songMutex_);
    return endBars(song_);
}

// ── Playback ──────────────────────────────────────────────────────────
bool SongEngine::Impl::playCountedIn(uint32_t fromBar, SyncTake take, uint8_t beats) {
    countInReq_.store(beats);
    if (playWithTake(fromBar, take)) return true;
    countInReq_.store(0);
    return false;
}

uint8_t SongEngine::Impl::countInBeatsLeft() const {
    switch (play_.load()) {
        case Play::Priming:  return cinPlanned_.load();
        case Play::Counting: return cinBeatsLeft_.load(std::memory_order_relaxed);
        case Play::Armed:    return syncing_.load() ? clk_.countInBeatsLeft() : 0;
        default:             return 0;
    }
}

bool SongEngine::Impl::playWithTake(uint32_t fromBar, SyncTake take) {
    if (!ensureWorker() || !yieldFreeze()) return false;   // a bounce owns the transport
    /* A seq track's length is its scene's, which the seq view changes
     * without a commit: taken again from a stop, not from the last one. */
    if (!playing()) {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        rebuild();
    }
    {
        std::lock_guard<std::mutex> l(snapMutex_);
        if (!snap_.end && !snap_.loopB) return false;
    }
    playFrom_.store(fromBar);
    playTake_.store(take);
    stopReq_.store(false);
    playReq_.store(true);
    p.wake();
    return true;
}

void SongEngine::Impl::stop() {
    if (!workerStarted_.load()) return;
    playReq_.store(false);
    stopReq_.store(true);
    p.wake();
}

bool SongEngine::Impl::playing() const {
    const Play p = play_.load();
    return p == Play::Priming || p == Play::Armed || p == Play::Counting || p == Play::Running;
}

double SongEngine::Impl::positionBars() const {
    const uint32_t spb = spBar_.load();
    return spb ? (double)pos_.load() / spb : 0.0;
}

/// Stop playback, cancel any bounce and give both rings back, before the
/// engine arena they may live in is wiped. Waits for song_io; never call
/// it from song_io or while holding the sample player's locks, which the
/// worker's card reads take.
void SongEngine::Impl::releaseRings() {
    if (!workerStarted_.load()) return;
    bCancel_.store(true);
    playReq_.store(false);
    stopReq_.store(true);
    releaseReq_.store(true);
    p.wake();
    /* Until it is done, however long a card job takes: the caller wipes
     * the arena next, and a ring still in it would be written by RT into
     * memory the pitched bank owns, then freed as a foreign pointer. */
    for (uint32_t waited = 0; releaseReq_.load(); waited += kReleasePollMs) {
        p.sleepMs(kReleasePollMs);
        if (waited && waited % kReleaseWaitMs == 0) {
            logf(true, "rings still not released after %u ms (song_io busy on the card)",
                 (unsigned)waited);
        }
    }
}

// ── the clip launcher's live transport ───────────────────────────────
bool SongEngine::Impl::liveStart() {
    if (!opened_.load() || !ensureWorker()) return false;
    liveStartReq_.store(true);
    p.wake();
    return true;
}

bool SongEngine::Impl::liveLaunch(uint8_t track, const std::string& file, uint16_t bars) {
    if (track >= kTracks || file.empty() || !opened_.load() || !ensureWorker()) return false;
    std::string path;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        path = song_.clipPath(file);
    }
    {
        std::lock_guard<std::mutex> l(liveMutex_);
        LiveReq& r = liveReq_[track];
        r.path = path;
        r.name = file;
        r.bars = bars ? bars : 1;
        r.stop = false;
        r.set  = true;
    }
    /* The first launch starts the transport: the request is already in,
     * so it lands on the first sample the grid plays. */
    if (!liveOn_.load()) liveStartReq_.store(true);
    p.wake();
    return true;
}

bool SongEngine::Impl::liveStop(uint8_t track) {
    if (!liveOn_.load()) return false;
    {
        std::lock_guard<std::mutex> l(liveMutex_);
        for (int t = 0; t < kTracks; ++t) {
            if (track < kTracks && t != (int)track) continue;
            liveReq_[t] = LiveReq{};
            liveReq_[t].stop = true;
            liveReq_[t].set  = true;
        }
    }
    p.wake();
    return true;
}

bool SongEngine::Impl::liveState(ISongEngine::LiveState& out) {
    out = ISongEngine::LiveState{};
    const uint32_t spb = liveSpb_.load();
    out.on = liveOn_.load();
    out.quantBars = liveQuant_.load();
    out.framesPerBar = spb;
    if (!out.on) return true;
    const uint32_t pos = pos_.load(std::memory_order_relaxed);
    out.bars = spb ? (double)pos / (double)spb : 0.0;
    std::lock_guard<std::mutex> l(liveMutex_);
    for (int t = 0; t < kTracks; ++t) {
        ISongEngine::LiveTrack& o = out.tracks[t];
        const LiveSlot& c = liveCur_[t];
        o.file       = c.name;
        o.bars       = c.bars;
        o.loopFrames = c.loop;
        /* The worker fills the ring ahead of what is heard, so a clip
         * applied a moment ago can start later than the audible
         * position: nothing into its loop yet, not a huge phase. */
        o.phase = c.loop && pos > c.startAt ? (pos - c.startAt) % c.loop : 0;
        if (liveReq_[t].set) {
            o.stopping = liveReq_[t].stop;
            o.pending  = liveReq_[t].stop ? std::string() : liveReq_[t].name;
        }
    }
    return true;
}

/// @p song as sample ranges. A later block cuts the one it lands on, as
/// on any timeline, and the song's length -- or the loop region, where it
/// reaches further -- cuts everything past it.
/// AudioSong::playEnd(), and with no length set at least one pass of the
/// seq track's scene: that is how long a song of only a seq track is.
uint32_t SongEngine::Impl::endBars(const AudioSong& song) const {
    return song_end_bars(song, scenes());
}

Arrangement SongEngine::Impl::flatten(const AudioSong& song) const {
    Arrangement a;
    a.samplesPerBar = song.samplesPerBar(sampleRate_);
    a.loop = song.loop;
    a.end = endBars(song) * a.samplesPerBar;
    /* A take running past the song's last block runs the song on with
     * it: at the old end it would wrap to bar one, or stop. */
    if (takeHorizon_) a.end = std::max<uint32_t>(a.end, kTakeHorizonBars * a.samplesPerBar);
    if (song.hasRegion()) {
        a.loopA = song.loopStart * a.samplesPerBar;
        a.loopB = song.loopEnd * a.samplesPerBar;
    }
    const uint32_t cut = std::max(a.end, a.loopB);
    for (int t = 0; t < kTracks; ++t) {
        const int route = song.seqRoute(t);
        if (route < 0) continue;
        /* Frozen: its file, as an audio track's block, and no notes. */
        if (frozenPlays(song, t)) {
            for (const SongClip& fz : song.tracks[t].freeze) {
                Segment s;
                s.start = fz.bar * a.samplesPerBar;
                s.end = std::min<uint32_t>((fz.bar + fz.bars) * a.samplesPerBar, cut);
                s.offset = fz.offset;
                s.path = song.clipPath(fz.file);
                if (s.end > s.start) a.tracks[t].push_back(s);
            }
            a.seqMapped = true;   // an empty map: the sequencer plays none of it
            continue;
        }
        /* A kit of its own the second kit engine does not hold now: nothing
         * to play its notes, until it is frozen or given the engine. */
        if (song.liveRoute(t) < 0) continue;
        /* On the song's grid: the song sets the clock's tempo when it
         * starts the sequencer with it. A track that plays its scene
         * everywhere is one span of it over the whole song -- laid on the
         * song like a cut one, so muting it silences its notes as it
         * plays (applySeqSound()). Each track's notes on the route of the
         * engine they play through. */
        if (song.tracks[t].seqBlocks) {
            auto spans = seqSpansOf(song.tracks[t], a.samplesPerBar, song.seqTicksPerBar(), cut, (uint8_t)route);
            a.seqSpans.insert(a.seqSpans.end(), std::make_move_iterator(spans.begin()),
                              std::make_move_iterator(spans.end()));
        } else {
            PatternSequencer::SongSpan all;
            all.b = cut;
            all.route = (uint8_t)route;
            /* The clock plays one scene, the pads' kit's: a Pitch track's
             * goes round as notes of its own, as it is now. */
            if (route) {
                const SeqPatternPtr own = scenePattern(song.tracks[t].scene);
                const uint32_t bars = own->bars ? own->bars : 1;
                all.notes = songNotes(own, a.samplesPerBar, song.seqTicksPerBar());
                all.loop = bars * a.samplesPerBar;
            }
            a.seqSpans.push_back(std::move(all));
        }
        a.seqMapped = true;
    }
    /* The shortest pass a bake can have: a bar of one beat at the fastest
     * tempo a song may have. One made before the song slowed down is shorter
     * than a bar and still goes round. */
    const uint32_t minBakeFrames = min_bake_frames(sampleRate_);
    const float bpm = song.bpm > 0.0f ? song.bpm : AudioSong::kDefaultBpm;
    const uint32_t beat = song.beatsPerBar ? a.samplesPerBar / song.beatsPerBar : 0;
    for (int t = 0; t < kTracks; ++t) {
        if (song.tracks[t].kind == TrackKind::Seq) continue;   // no audio of its own
        const auto& cs = song.tracks[t].clips;
        for (size_t i = 0; i < cs.size(); ++i) {
            Segment s;
            s.start = bars_to_frames(cs[i].bar, a.samplesPerBar);
            s.end   = clip_block_end(cs, i, a.samplesPerBar, cut);
            /* An element goes round its bake, a segment a pass: the first
             * from the element bar the block starts on, the rest whole. Not
             * baked yet (no file, no length) it plays nothing. Never fitted:
             * the bake is made at the song's tempo -- one made at another,
             * before the tempo changed, plays varispeed, its passes the
             * element's bars at this one, until the relink pass links the
             * bake at this tempo. A pass shorter than any bake can be is no
             * bake (a song.json cut or edited by hand): as passes it would
             * be millions of segments, under the song's lock. */
            if (cs[i].isElement()) {
                if (cs[i].file.empty() || cs[i].frames < minBakeFrames) continue;
                s.path = bakePath(cs[i].file);
                const uint32_t pass = varispeed_frames(cs[i].frames, cs[i].bakeBpm, bpm, beat);
                s.step = varispeed_step(cs[i].frames, pass);
                element_passes(cs[i].from, pass, a.samplesPerBar, s.start, s.end,
                               [&](uint32_t from, uint32_t to, uint32_t offset) {
                    Segment pass = s;
                    pass.start = from;
                    pass.end = to;
                    pass.offset = offset;
                    a.tracks[t].push_back(pass);
                });
                continue;
            }
            /* A stretched track plays its block's fitted copy, which fills
             * the block's bars from its first frame -- once song_io has
             * made it. Known from memory, not the card: this runs under
             * the song's lock, on whatever thread committed. */
            const StretchMode mode = song.tracks[t].stretch;
            const std::string fitted = mode != StretchMode::Off
                ? song.clipPath(stretchName(cs[i], cs[i].bars * a.samplesPerBar, mode)) : std::string();
            if (!fitted.empty() && stretchReady(fitted)) {
                s.path = fitted;
            } else {
                s.path = song.clipPath(cs[i].file);
                if (cs[i].frames) s.end = std::min(s.end, s.start + cs[i].frames);
                s.offset = cs[i].offset;
            }
            if (s.end > s.start) a.tracks[t].push_back(s);
        }
    }
    return a;
}

std::string SongEngine::Impl::bakePath(const std::string& file) const {
    return std::string(p.songsRoot()) + "/" + AudioSong::bakeDir() + "/" + file;
}

bool SongEngine::Impl::readAt(ClipReader& r, uint32_t clipFrame, uint64_t step, int16_t* dst, uint32_t n) {
    if (!step) return r.readWhole(clipFrame, dst, n);
    /* The file's position is the song's: a chunk goes on exactly where the last
     * one ended, and a loop or a locate lands where it should. */
    return r.readVarispeed((uint64_t)clipFrame * step, step, dst, n, vsrc_, sz.chunkFrames + kVarispeedGuard);
}

/// A seq track's blocks as stretches of the song at @p spb samples a
/// bar (@p tpb ticks): each from its bar to its end or the next block's,
/// up to @p cut, with its own notes as samples -- or, a block that plays
/// the scene, the scene sample its bar of the scene starts on. On
/// @p route 0 that is the scene sounding; on another, which never has the
/// clock's scene, the track's own scene as it is now, as notes.
std::vector<PatternSequencer::SongSpan> SongEngine::Impl::seqSpansOf(const SongTrack& tr, uint32_t spb, uint32_t tpb,
                                                                     uint32_t cut, uint8_t route) const {
    std::vector<PatternSequencer::SongSpan> out;
    const uint8_t sb = seq_.sceneBars(tr.scene);
    SeqPatternPtr own;   // the track's scene, for a block of it on another route
    const auto& cs = tr.clips;
    for (size_t i = 0; i < cs.size(); ++i) {
        uint32_t b = (cs[i].bar + cs[i].bars) * spb;
        if (i + 1 < cs.size()) b = std::min(b, cs[i + 1].bar * spb);
        if (cut) b = std::min(b, cut);
        const uint32_t a = cs[i].bar * spb;
        if (b <= a) continue;
        PatternSequencer::SongSpan s;
        s.a = a;
        s.b = b;
        s.route = route;
        if (!cs[i].notes && route && !own) own = scenePattern(tr.scene);
        const SeqPatternPtr& notes = cs[i].notes || !route ? cs[i].notes : own;
        if (notes) {
            const uint32_t bars = notes->bars ? notes->bars : 1;
            s.notes = songNotes(notes, spb, tpb);
            s.loop = bars * spb;
            s.scene = (uint32_t)(cs[i].from % bars) * spb;
        } else {
            s.scene = (uint32_t)(cs[i].from % (sb ? sb : 1)) * spb;
        }
        out.push_back(std::move(s));
    }
    return out;
}

/// @p p as the sequencer plays it, at @p spb samples (@p tpb ticks) a
/// bar. Made once per pattern and tempo: a block's copies share it, and
/// a commit that left the notes alone -- a fader -- converts nothing.
PatternSequencer::SongNotes SongEngine::Impl::songNotes(const SeqPatternPtr& p, uint32_t spb, uint32_t tpb) const {
    std::lock_guard<std::mutex> l(noteCacheMutex_);
    for (const auto& e : noteCache_)
        if (e.pattern == p && e.spb == spb && e.tpb == tpb) return e.notes;
    auto v = std::make_shared<std::vector<PatternSequencer::SongNote>>();
    v->reserve(p->notes.size());
    const uint32_t t = tpb ? tpb : 1;
    for (const SeqNote& n : p->notes) {
        PatternSequencer::SongNote o;
        o.pos = (uint32_t)((uint64_t)n.tick * spb / t);
        o.len = (uint32_t)((uint64_t)n.len * spb / t);
        o.pad = n.pad;
        o.vel = n.vel;
        o.perf = n.perf;
        v->push_back(o);
    }
    if (noteCache_.size() >= kNoteCacheSize) noteCache_.erase(noteCache_.begin());
    noteCache_.push_back({p, spb, tpb, v});
    return v;
}

/// The sequencer laid on what the song plays now: the seq track's blocks,
/// wrapping where the song does -- or, with none, its own loop. Worker.
void SongEngine::Impl::pushSeqMap() {
    std::vector<PatternSequencer::SongSpan> spans;
    bool mapped = false;
    uint32_t wrapFrom = 0, wrapTo = 0;
    {
        std::lock_guard<std::mutex> l(snapMutex_);
        mapped = snap_.seqMapped;
        if (mapped) spans = snap_.seqSpans;
        const bool region = snap_.loopB > snap_.loopA;
        wrapFrom = snap_.loop ? (region ? snap_.loopB : snap_.end) : 0;
        wrapTo = region ? snap_.loopA : 0;
        mapSentGen_ = snapGen_.load();
    }
    if (mapped) seq_.setSongMap(spans, wrapFrom, wrapTo);
    else        seq_.clearSongMap();
}

/// Worker: track @p t's loudest frames @p m (left, right) in hop @p hop
/// of the ring.
void SongEngine::Impl::meterPut(int t, uint32_t hop, const int32_t m[2]) {
    Meter& s = meter_[hop % kMeterSlots];
    if (s.hop.load(std::memory_order_relaxed) != hop) {
        for (auto& tr : s.peak) for (auto& p : tr) p.store(0, std::memory_order_relaxed);
        s.hop.store(hop, std::memory_order_release);
    }
    for (int c = 0; c < 2; ++c) {
        const uint16_t v = (uint16_t)std::min<int32_t>(m[c], 32767);
        if (v > s.peak[t][c].load(std::memory_order_relaxed)) s.peak[t][c].store(v, std::memory_order_relaxed);
    }
}

/// A ring that starts again from frame 0 has no meters yet.
void SongEngine::Impl::meterReset() {
    for (auto& s : meter_) s.hop.store(UINT32_MAX, std::memory_order_relaxed);
}

/// The song for the worker, and the per-track gains and loop for RT --
/// so a mute or a fader acts at once while playing; a relink pass asked for
/// when element blocks or the tempo moved. Caller holds songMutex_.
void SongEngine::Impl::rebuild() {
    noteChange();
    std::string sig = elementSignature(song_);
    if (sig != elementSig_) {
        elementSig_ = std::move(sig);
        relinkReq_.store(true);
    }
    Arrangement a = flatten(song_);
    a.context = contextSegments();
    ctxApart_.store(contextApart());
    for (int t = 0; t < kTracks; ++t) {
        const auto& tr = song_.tracks[t];
        gain_[t].store(song_.silenced(t) ? 0.0f : tr.volume);
        pan_[t].store(tr.pan);
    }
    for (int t = 0; t < kTracks; ++t) seqRoute_[t].store((int8_t)song_.liveRoute(t), std::memory_order_relaxed);
    /* The live transport has a grid of its own and no end; a commit
     * made while it runs (a fader, a mute) must not hand RT the
     * arrangement's. */
    const bool live = liveOn_.load();
    if (!live) {
        loop_.store(a.loop);
        spBar_.store(a.samplesPerBar);
    }
    {
        std::lock_guard<std::mutex> l(snapMutex_);
        if (!live) {
            end_.store(a.end);
            loopA_.store(a.loopA);
            loopB_.store(a.loopB);
        }
        snap_ = std::move(a);
        snapGen_.fetch_add(1);
    }
    applySeqSound();
}

/// The seq tracks as the arrangement has them while the song plays:
/// each one's fader and pan on the engine its notes play through
/// (setSeqGain / setKitGain, setSeqPan / setKitPan), and its notes silent while it is muted or another track
/// is soloed -- the pads a player hits stay theirs -- or under a REC,
/// which replaces them. A route at a time: the other kit track plays on.
/// Stopped, the engines as the Mixer has them. Caller holds songMutex_.
void SongEngine::Impl::applySeqSound() {
    const bool on = playing();
    uint8_t muted = 0;
    float gain[4] = {1.0f, 1.0f, 1.0f, 1.0f};   // the pads' kit, the instrument, the kit slots' engines
    float pan[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int t = 0; t < kTracks; ++t) {
        const int route = song_.liveRoute(t);
        if (route < 0) continue;
        const SongTrack& tr = song_.tracks[t];
        if ((takeRecMute_ && t == takeTrack_) || (on && song_.silenced(t))) muted |= (uint8_t)(1u << route);
        if (on) {
            const int e = tr.source == SeqSource::Pitch ? 1 : route == 3 ? 3 : route ? 2 : 0;
            gain[e] = tr.volume;
            pan[e] = tr.pan;
        }
    }
    seq_.setSongRoutesMuted(muted);
    for (int i = 0; i < 4; ++i) {
        const uint8_t kitRoute = i == 3 ? 3 : i == 2 ? 1 : 0;
        if (gain[i] != seqGain_[i]) {
            seqGain_[i] = gain[i];
            if (i == 1) p.setSeqGain(SeqSource::Pitch, gain[i]);
            else        p.setKitGain(kitRoute, gain[i]);
        }
        if (pan[i] != seqPan_[i]) {
            seqPan_[i] = pan[i];
            if (i == 1) p.setSeqPan(SeqSource::Pitch, pan[i]);
            else        p.setKitPan(kitRoute, pan[i]);
        }
    }
}

// ── Stretch ───────────────────────────────────────────────────────────
bool SongEngine::Impl::stretchReady(const std::string& path) const {
    std::lock_guard<std::mutex> l(stretchMutex_);
    return stretchReady_.count(path) != 0;
}

/// The blocks of stretched tracks still without a copy, and the first of
/// them in @p job. What stretchWaiting() answers.
uint32_t SongEngine::Impl::nextStretch(StretchJob* job) {
    std::lock_guard<std::recursive_mutex> l(songMutex_);
    if (!opened_) return 0;
    const uint32_t spb = song_.samplesPerBar(sampleRate_);
    uint32_t pending = 0;
    std::lock_guard<std::mutex> s(stretchMutex_);
    for (const auto& tr : song_.tracks) {
        if (!tr.inUse || tr.stretch == StretchMode::Off || tr.kind != TrackKind::Audio) continue;
        for (const auto& c : tr.clips) {
            if (c.file.empty() || c.isElement()) continue;   // an element's bake is made at the song's tempo
            const std::string dst = song_.clipPath(stretchName(c, c.bars * spb, tr.stretch));
            if (stretchReady_.count(dst) || stretchSkip_.count(dst)) continue;
            if (!pending++ && job) {
                job->src = song_.clipPath(c.file);
                job->dst = dst;
                job->offset = c.offset;
                job->frames = c.frames;
                job->outFrames = c.bars * spb;
                job->repitch = tr.stretch == StretchMode::Repitch;
            }
        }
    }
    return pending;
}

/// Worker: the first block still without its copy -- read, stretched or
/// repitched (TimeStretch) and written beside the clips, renamed in when
/// whole.
/// Only while nothing plays; a request coming in stops it, and it starts
/// again from the beginning after.
void SongEngine::Impl::doStretch() {
    StretchJob j;
    if (!nextStretch(&j)) return;
    auto skip = [&](const char* why) {
        std::lock_guard<std::mutex> l(stretchMutex_);
        stretchSkip_.insert(j.dst);
        if (why) logf(true, "stretch %s: %s", j.dst.c_str(), why);
    };
    auto ready = [&]() {
        {
            std::lock_guard<std::mutex> l(stretchMutex_);
            stretchReady_.insert(j.dst);
        }
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        rebuild();
    };
    if (fsx::exists(j.dst)) { ready(); return; }
    ClipReader r;
    if (!r.open(j.src)) { skip("cannot read the clip"); return; }
    const uint32_t rest = r.frames > j.offset ? r.frames - j.offset : 0;
    const uint32_t in = j.frames ? std::min(j.frames, rest) : rest;
    if (!in) { skip("nothing to stretch"); return; }
    const double ratio = (double)j.outFrames / in;
    if (std::fabs(ratio - 1.0) < kStretchSame) { skip(nullptr); return; }
    if (ratio < TimeStretch::kMinRatio || ratio > TimeStretch::kMaxRatio) {
        skip("too far from the song's tempo");
        return;
    }
    /* The ring segments' allocator: the engine arena first, and never the
     * last of the PSRAM the UI lives in. Given back below, and before any
     * arena reset -- releaseRings() is a request, which stops the run. */
    void* work = p.allocRingSegment(TimeStretch::workBytes(), 1);
    if (!work) { skip("not enough memory"); return; }
    const size_t slash = j.dst.find_last_of('/');
    fsx::makeDir(j.dst.substr(0, slash));
    const std::string tmp = j.dst + ".tmp";
    WavWriter w;
    if (!w.open(tmp.c_str(), sampleRate_, 2)) {
        p.freeRingSegment(work);
        skip("card write failed");
        return;
    }
    struct Ctx { Impl* self; ClipReader* r; WavWriter* w; uint32_t offset; uint32_t hops; };
    Ctx ctx{this, &r, &w, j.offset, 0};
    stretchProg_.store(0);
    const bool ok = (j.repitch ? TimeStretch::repitch : TimeStretch::run)(in, j.outFrames, work,
        [](void* c, uint32_t frame, int16_t* dst, uint32_t n) {
            auto* x = static_cast<Ctx*>(c);
            x->r->read(x->offset + frame, dst, n);
            return true;
        },
        [](void* c, const int16_t* src, uint32_t n) { return static_cast<Ctx*>(c)->w->write(src, n); },
        [](void* c, uint32_t done, uint32_t total) {
            /* The streamer and the screen first: a breath every few hops,
             * and out at once when anything else is asked of song_io. */
            auto* x = static_cast<Ctx*>(c);
            x->self->stretchProg_.store((uint8_t)((uint64_t)done * 100 / (total ? total : 1)));
            if ((++x->hops & 3) == 0) x->self->p.sleepMs(1);
            if ((x->hops & 15) == 0) x->self->p.yieldIo();
            return !x->self->requestWaiting();
        },
        &ctx);
    p.freeRingSegment(work);
    r.close();
    const bool closed = w.close();
    if (!ok || !closed || !fsx::rename(tmp, j.dst)) {
        std::remove(tmp.c_str());
        if (ok) skip("card write failed");   // stopped by a request: tried again after it
        return;
    }
    logf(false, "%s %s x%.3f", j.repitch ? "repitched" : "stretched", j.dst.c_str(), ratio);
    ready();
}

/// Worker: the live transport -- the clip launcher's. No arrangement:
/// each track plays whatever was launched on it, looped on the song's bar
/// grid, and the position simply runs. The cycle is a whole number of
/// bars just under where the sample counter wraps, which is a day at any
/// tempo, so a loop's phase is plain subtraction and nothing has to be
/// re-anchored while anybody is playing.
void SongEngine::Impl::startLive() {
    /* A launch made with the transport stopped is what starts it, so the
     * request it queued has to survive the stop endPlayback() performs
     * here -- otherwise the grid starts with nothing on it. */
    LiveReq keep[kTracks];
    {
        std::lock_guard<std::mutex> l(liveMutex_);
        for (int t = 0; t < kTracks; ++t) keep[t] = liveReq_[t];
    }
    endPlayback();
    {
        std::lock_guard<std::mutex> l(liveMutex_);
        for (int t = 0; t < kTracks; ++t) liveReq_[t] = keep[t];
    }
    uint32_t spb = 0;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        spb = song_.samplesPerBar(sampleRate_);
        /* The song's faders and mutes are the launcher's too; they are
         * only published on a commit, and a project that has had none
         * since it was opened would play at gain zero. */
        for (int t = 0; t < kTracks; ++t) {
            const auto& tr = song_.tracks[t];
            gain_[t].store(song_.silenced(t) ? 0.0f : tr.volume);
            pan_[t].store(tr.pan);
        }
    }
    if (!spb) return;
    if (!ring_.alloc(sz.songRingFrames, kPrimeFrames)) {
        logf(true, "live: not enough memory for the song ring (%u kB free)",
             (unsigned)(p.freeMemoryBytes() / 1024));
        return;
    }
    meterReset();
    liveSpb_.store(spb);
    liveCycle_.store((0xFFFF0000u / spb) * spb);
    writePos_ = 0;
    debt_.store(0);
    pos_.store(0);
    startPos_ = 0;
    end_.store(liveCycle_.load());
    loop_.store(true);
    loopA_.store(0);
    loopB_.store(0);
    spBar_.store(spb);
    syncing_ = false;
    take_ = SyncTake::None;
    cinPlanned_.store(0);
    countInReq_.store(0);
    underruns_.store(0);
    openFailures_.store(0);
    played_.store(0);
    liveOn_.store(true);
    job_.store(Job::Play);
    play_.store(Play::Priming, std::memory_order_seq_cst);
}

/// Worker: a launch or a stop takes effect, on sample @p at. Caller holds
/// liveMutex_.
void SongEngine::Impl::liveApply(int t, uint32_t at) {
    LiveReq& r = liveReq_[t];
    if (!r.set) return;
    r.set = false;
    if (r.stop || r.path.empty()) { liveCur_[t] = LiveSlot{}; return; }
    liveCur_[t].path    = r.path;
    liveCur_[t].name    = r.name;
    liveCur_[t].bars    = r.bars ? r.bars : 1;
    liveCur_[t].loop    = liveCur_[t].bars * liveSpb_.load();
    liveCur_[t].startAt = at;
}

/// Worker: from the snapshot, @p fromBar on. In sync the sequencer is the
/// song's: at the song's tempo (its pattern is rescaled with it, so a take
/// keeps its place in the bar), and stopped until the song starts it.
void SongEngine::Impl::startPlayback(uint32_t fromBar) {
    endPlayback();
    bool sync = false;
    float bpm = 0.0f;
    int seqScene = -1;
    {
        /* A seq track is the sequencer playing its scene with the song --
         * SYNC on or off; SYNC on without one plays the scene in use. */
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        /* Muted or not: its mute silences its notes as the song plays
         * (applySeqSound()), so unmuting it mid-song brings them back. */
        const bool take = playTake_.load() != SyncTake::None || seqTake_.load() != SyncTake::None;
        /* The scene the clock runs: the one the seq view edits while it is
         * open -- a part recorded there goes into its block, whichever kit
         * track it is on -- else the pads' track's, which a block playing
         * the scene that sounds plays. */
        const int t = song_.seqTrackOnRoute(0);
        if (editTrack_ >= 0)  seqScene = editScene_;
        else if (t >= 0)      seqScene = song_.tracks[t].scene;
        sync = song_.syncSeq || song_.seqTracks() > 0 || take;
        bpm = song_.bpm;
    }
    {
        /* The faders and mutes, published before the worker reads them.
         * fillRings() skips a track whose gain is <= 0 and gain_ starts
         * at zero, so a project that has had no commit since it was
         * opened filled the ring with silence: the position advanced, the
         * ring never underran and no file was ever opened, which is
         * exactly what the Arranger looked like -- a playhead moving over
         * a song nobody could hear. The live transport already did this
         * (see liveStart) and said why; this path did not. */
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        for (int t = 0; t < kTracks; ++t) {
            const auto& tr = song_.tracks[t];
            gain_[t].store(song_.silenced(t) ? 0.0f : tr.volume);
            pan_[t].store(tr.pan);
        }
    }
    {
        std::lock_guard<std::mutex> l(snapMutex_);
        if (!snap_.end && !snap_.loopB) return;
        const uint32_t from = std::min(fromBar * snap_.samplesPerBar,
                                       std::max(snap_.end, snap_.loopB) - 1);
        if (!ring_.alloc(sz.songRingFrames, kPrimeFrames)) {
            logf(true, "play: not enough memory for the song ring (%u kB free)",
                 (unsigned)(p.freeMemoryBytes() / 1024));
            return;
        }
        /* Past the ring's minimum: without it the tracks read a chunk at a time. */
        ahead_ = static_cast<uint8_t*>(p.allocRingSegment((size_t)kAheadBytes * kTracks, 0));
        for (int t = 0; t < kTracks; ++t)
            readers_[t].setAhead(ahead_ ? ahead_ + (size_t)t * kAheadBytes : nullptr, kAheadBytes);
        meterReset();
        writePos_ = from;
        debt_.store(0);
        pos_.store(from);
        startPos_ = from;
    }
    syncing_ = sync;
    take_ = sync ? playTake_.load() : SyncTake::None;
    /* Asked for with this play only; a plain play after it counts none. */
    const uint8_t beats = countInReq_.exchange(0);
    cinPlanned_.store(beats);
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        cinBpb_ = song_.beatsPerBar ? song_.beatsPerBar : AudioSong::kDefaultBeatsPerBar;
        cinBeat_ = song_.samplesPerBar(sampleRate_) / cinBpb_;
    }
    if (sync) {
        PatternSequencer& seq = seq_;
        seq.stop();
        if (bpm > 0.0f && std::fabs(seq.getClickBpm() - bpm) > 0.01f) seq.setClickBpm(bpm);
        /* Stopped, the selection takes at once. */
        if (seqScene >= 0) seq.selectScene((uint8_t)seqScene);
    }
    underruns_.store(0);
    openFailures_.store(0);
    played_.store(0);
    job_.store(Job::Play);
    play_.store(Play::Priming, std::memory_order_seq_cst);
    std::lock_guard<std::recursive_mutex> l(songMutex_);
    applySeqSound();
}

void SongEngine::Impl::endPlayback() {
    lastFillUs_ = 0;
    /* A take this playback ran lands before the sequencer stops: where
     * the song is when it stops is where the take ends. */
    if (takeBound_) {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        finishTake();
        takeBound_ = false;
    }
    if (play_.load() == Play::Idle && job_.load() != Job::Play) return;
    play_.store(Play::Idle, std::memory_order_seq_cst);
    {
        /* The engines back to the Mixer's level, and the notes heard. */
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        applySeqSound();
    }
    /* The song took the sequencer along; it takes it back down. */
    if (syncing_) {
        seq_.stop();
        seq_.clearSongMap();
    }
    syncing_ = false;
    waitRtOut();
    played_.store(0);
    liveOn_.store(false);
    {
        /* Neither what played nor what was launched outlives a stop: a
         * clip left in a request would fire on the next start. */
        std::lock_guard<std::mutex> l(liveMutex_);
        for (int t = 0; t < kTracks; ++t) { liveCur_[t] = LiveSlot{}; liveReq_[t] = LiveReq{}; }
    }
    for (auto& r : readers_) {
        r.close();
        r.setAhead(nullptr, 0);
    }
    if (ahead_) {
        p.freeRingSegment(ahead_);
        ahead_ = nullptr;
    }
    ctxReader_.close();
    ring_.release();
    job_.store(Job::None);
}

/// Keep the song ring full: each chunk of song time is every unmuted
/// track's blocks read from the card and summed at its gain, and the context
/// stem under them at unity unless SOLO is on. What is not under a block is
/// silence and costs no read.
void SongEngine::Impl::fillRings() {
    int16_t* chunk = io_;
    int32_t* acc = acc_;
    if (!chunk || !acc) return;
    {
        /* Time off the ring: what the worker did between two top-ups. */
        const int64_t now = p.nowUs();
        if (lastFillUs_) dbgMax(dbgGapMaxUs_, now - lastFillUs_);
        lastFillUs_ = now;
    }
    /* An edit while it plays moves the seq blocks with the audio. */
    if (syncing_ && play_.load() != Play::Priming && snapGen_.load() != mapSentGen_) pushSeqMap();
    while (ring_.space() >= sz.chunkFrames) {
        if (stopReq_.load()) return;
        uint32_t wp = writePos_;
        uint32_t n = sz.chunkFrames;
        /* Where each track's blocks fall in this chunk (pieces_). */
        Piece (&pieces)[kTracks][3] = pieces_;
        int pieceCount[kTracks] = {};
        Piece ctxPieces[3];
        int ctxCount = 0;
        if (liveOn_.load()) {
            /* The launcher: the grid runs on by itself and every track
             * reads its clip at the phase the position puts it at. A
             * launch waiting for a line cuts the chunk exactly there, so
             * the next one starts with the new clips -- which is what
             * makes a whole row land on one sample. */
            std::lock_guard<std::mutex> l(liveMutex_);
            const uint32_t cycle = liveCycle_.load();
            const uint32_t spb = liveSpb_.load();
            if (!cycle || !spb) return;
            wp += debt_.exchange(0);
            if (wp >= cycle) wp %= cycle;
            n = std::min(n, cycle - wp);
            const uint8_t qb = liveQuant_.load();
            const uint32_t q = (qb ? qb : 1) * spb;
            bool waiting = false;
            for (int t = 0; t < kTracks; ++t) waiting = waiting || liveReq_[t].set;
            if (waiting) {
                if (wp % q == 0) for (int t = 0; t < kTracks; ++t) liveApply(t, wp);
                else n = std::min(n, q - wp % q);
            }
            for (int t = 0; t < kTracks; ++t) {
                LiveSlot& c = liveCur_[t];
                if (!c.loop || gain_[t].load(std::memory_order_relaxed) <= 0.0f) continue;
                if (c.startAt > wp) c.startAt = wp;        // once a day, at the wrap
                uint32_t off = (wp - c.startAt) % c.loop;
                uint32_t at = 0, left = n;
                while (left && pieceCount[t] < 3) {
                    const uint32_t run = std::min(left, c.loop - off);
                    Piece& pc = pieces[t][pieceCount[t]++];
                    std::snprintf(pc.path, sizeof(pc.path), "%s", c.path.c_str());
                    pc.at = at;
                    pc.len = run;
                    pc.clipFrame = off;
                    pc.step = 0;
                    at += run;
                    left -= run;
                    off = 0;
                }
            }
        } else {
            std::lock_guard<std::mutex> l(snapMutex_);
            const uint32_t end = snap_.end;
            const uint32_t lim = snap_.loop && snap_.loopB > snap_.loopA ? snap_.loopB : end;
            if (!lim) return;
            /* Skip what RT already played as silence. */
            wp += debt_.exchange(0);
            if (wp >= lim) {
                if (!snap_.loop) { writePos_ = lim; break; }
                wp = wrap_position(wp, end, snap_.loopA, snap_.loopB);
            }
            n = std::min(n, lim - wp);
            auto gather = [&](const std::vector<Segment>& segs, Piece* into, int& count) {
                for (const auto& sg : segs) {
                    if (sg.end <= wp) continue;
                    if (sg.start >= wp + n || count == 3) break;
                    const uint32_t a = std::max(sg.start, wp);
                    const uint32_t b = std::min(sg.end, wp + n);
                    Piece& pc = into[count++];
                    std::snprintf(pc.path, sizeof(pc.path), "%s", sg.path.c_str());
                    pc.at = a - wp;
                    pc.len = b - a;
                    pc.clipFrame = a - sg.start + sg.offset;
                    pc.step = sg.step;
                }
            };
            for (int t = 0; t < kTracks; ++t) {
                if (gain_[t].load(std::memory_order_relaxed) <= 0.0f) continue;
                gather(snap_.tracks[t], pieces[t], pieceCount[t]);
            }
            if (!ctxSolo_.load(std::memory_order_relaxed)) gather(snap_.context, ctxPieces, ctxCount);
        }
        std::memset(acc, 0, n * 8);
        /* Where this chunk's first frame lands in the ring, which is how
         * RT will count it when it plays it: the meters' clock. */
        const uint32_t at0 = ring_.head.load(std::memory_order_relaxed);
        for (int t = 0; t < kTracks; ++t) {
            /* A track with nothing under this chunk gives its file back:
             * the sample player shares the card's handful of descriptors,
             * and a pad that cannot open its sample goes silent. */
            if (!pieceCount[t] && readers_[t].fd >= 0) readers_[t].close();
            const float g = gain_[t].load(std::memory_order_relaxed);
            /* The track's pan as the Mixer's: a balance, the side it
             * moves away from turned down; the meter reads it after. */
            const float pan = pan_[t].load(std::memory_order_relaxed);
            const float gs[2] = {g * (pan > 0.0f ? 1.0f - pan : 1.0f), g * (pan < 0.0f ? 1.0f + pan : 1.0f)};
            for (int i = 0; i < pieceCount[t]; ++i) {
                const Piece& pc = pieces[t][i];
                const int64_t r0 = p.nowUs();
                const int64_t tOpen = readers_[t].path != pc.path ? p.nowUs() : 0;
                workerAt_.store(AtOpen, std::memory_order_relaxed);
                const bool opened = readers_[t].path == pc.path ||
                                    (p.storageReady() && readers_[t].open(std::string(pc.path)));
                workerAt_.store(AtRead, std::memory_order_relaxed);
                if (tOpen) dbgMax(dbgOpenMaxUs_, p.nowUs() - tOpen);
                if (!opened) {
                    openFailures_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                readAt(readers_[t], pc.clipFrame, pc.step, chunk, pc.len);
                workerAt_.store(AtFill, std::memory_order_relaxed);
                /* One card access (an open included): a long one is the
                 * card held by someone else. */
                dbgMax(dbgReadMaxUs_, p.nowUs() - r0);
                int32_t* d = acc + pc.at * 2;
                for (uint32_t k = 0; k < pc.len;) {
                    const uint32_t f = at0 + pc.at + k;
                    const uint32_t run = std::min(pc.len - k, kMeterHop - f % kMeterHop);
                    int32_t m[2] = {0, 0};
                    for (uint32_t j = k * 2; j < (k + run) * 2; ++j) {
                        const int32_t v = (int32_t)((float)chunk[j] * gs[j & 1]);
                        d[j] += v;
                        m[j & 1] = std::max(m[j & 1], v < 0 ? -v : v);
                    }
                    meterPut(t, f / kMeterHop, m);
                    k += run;
                }
                /* The card is the sampler's streamer's first -- while this
                 * ring has half of itself in hand. Below that it is the
                 * song that is about to run dry: yielding up to 3 ms a
                 * read with voices sounding, three tracks on a 1-bit card
                 * read slower than they played (ringmin 2848 frames,
                 * 09.10.2026). */
                if (ring_.used() >= ring_.cap / 2) {
                    workerAt_.store(AtYield, std::memory_order_relaxed);
                    p.yieldIo();
                    workerAt_.store(AtFill, std::memory_order_relaxed);
                }
            }
        }
        /* The context stem, as a track at unity with no meter: its descriptor
         * given back as soon as nothing of it is under the chunk, or SOLO is on. */
        if (!ctxCount && ctxReader_.fd >= 0) ctxReader_.close();
        for (int i = 0; i < ctxCount; ++i) {
            const Piece& pc = ctxPieces[i];
            const int64_t r0 = p.nowUs();
            if (ctxReader_.path != pc.path && !(p.storageReady() && ctxReader_.open(std::string(pc.path)))) {
                openFailures_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            readAt(ctxReader_, pc.clipFrame, pc.step, chunk, pc.len);
            dbgMax(dbgReadMaxUs_, p.nowUs() - r0);
            int32_t* d = acc + pc.at * 2;
            for (uint32_t k = 0; k < pc.len * 2; ++k) d[k] += chunk[k];
            if (ring_.used() >= ring_.cap / 2) {
                workerAt_.store(AtYield, std::memory_order_relaxed);
                p.yieldIo();
                workerAt_.store(AtFill, std::memory_order_relaxed);
            }
        }
        for (uint32_t k = 0; k < n * 2; ++k) {
            const int32_t v = acc[k];
            chunk[k] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
        ring_.push(chunk, n);
        writePos_ = wp + n;
    }
    if (play_.load() == Play::Priming && ring_.used() >= kPrimeFrames) {
        if (syncing_) {
            /* The pattern laid on the song's bars: the scene's bar one on
             * the song's bar one, so starting mid-song starts mid-scene
             * where the timeline says. RT starts the song with the clock. */
            const uint32_t at = startPos_;
            PatternSequencer& seq = seq_;
            MetronomeNode& clk = clk_;
            const uint32_t loop = clk.loopSamples();
            armPos_.store(loop ? at % loop : 0, std::memory_order_relaxed);
            /* Before the clock moves: a poll in between would fire a
             * window of the scene the blocks say is silent. */
            pushSeqMap();
            play_.store(Play::Armed, std::memory_order_release);
            /* A count-in is the metronome's: the scene waits in PreBeat
             * and the song for the clock to move, both on its last click. */
            seq.playbackAt(at, cinPlanned_.load());
            /* A take asked for with the song: opened with it, on the same
             * sample, and never with a count-in of its own. */
            if (take_ == SyncTake::Record)       seq.record();
            else if (take_ == SyncTake::Overdub) seq.overdub();
        } else if (const uint8_t beats = cinPlanned_.load()) {
            /* Alone the song counts itself in: the same click, on the
             * song's beat, and the song on the sample after the last. */
            cinTotal_ = cinLeft_ = beats * cinBeat_;
            clickLen_ = sampleRate_ / 50;
            clickLeft_ = 0;
            cinBeatsLeft_.store(beats, std::memory_order_relaxed);
            play_.store(Play::Counting, std::memory_order_release);
        } else {
            play_.store(Play::Running, std::memory_order_release);
        }
    }
}

} // namespace crosspad

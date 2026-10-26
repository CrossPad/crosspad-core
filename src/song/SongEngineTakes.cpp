// SPDX-License-Identifier: GPL-3.0-or-later
//
// SongEngine, the takes onto the seq track, and the seq view's one-block-at-a-time editing.
// The state is SongEngine::Impl (SongEngineImpl.hpp); the worker loop is in SongEngine.cpp.

#include "SongEngineImpl.hpp"

namespace crosspad {

// ── Takes onto the seq track (Arrange's REC and DUB) ──────────────────
bool SongEngine::Impl::recordSeq(uint32_t fromBar, SyncTake take, uint8_t beats) {
    if (take == SyncTake::None || playing() || !ensureWorker() || !yieldFreeze()) return false;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (!opened_ || seqTake_.load() != SyncTake::None) return false;
        if (!openTake(take, fromBar)) return false;
        /* Round a region, from its start: the passes are its laps. Round the
         * whole song it is from the playhead, as without a cycle. */
        if (takeCycle_ && !takeWhole_) fromBar = takeCycleA_;
        takeFrom_ = fromBar * song_.samplesPerBar(sampleRate_);
    }
    if (playCountedIn(fromBar, SyncTake::None, beats)) return true;
    std::lock_guard<std::recursive_mutex> l(songMutex_);
    closeTake();
    return false;
}

bool SongEngine::Impl::seqPunch(SyncTake take) {
    std::lock_guard<std::recursive_mutex> l(songMutex_);
    const SyncTake now = seqTake_.load();
    if (take == now) return true;
    /* Into a song playing with the sequencer locked to it: that is
     * what keeps the hits and counts where the song is. */
    if (take != SyncTake::None && !seqPunchable()) return false;
    if (now != SyncTake::None) {
        finishTake();
        takeBound_ = false;
    }
    if (take == SyncTake::None) return true;
    const uint32_t spb = song_.samplesPerBar(sampleRate_);
    if (!openTake(take, spb ? seq_.songMapPosition() / spb : 0)) return false;
    takeFrom_ = seq_.songMapPosition();
    takeBound_ = true;
    return true;
}

// ── The seq view of a block ───────────────────────────────────────────
bool SongEngine::Impl::openSeqEdit(uint8_t track, uint32_t bar, uint8_t scene) {
    std::lock_guard<std::recursive_mutex> l(songMutex_);
    closeSeqEdit();
    if (track >= kTracks || scene >= PatternSequencer::kNumSequences) return false;
    SongTrack& tr = song_.tracks[track];
    if (!tr.inUse || tr.kind != TrackKind::Seq) return false;
    /* Read before it is written over: the scene may be the very one
     * the track's older blocks play. */
    const int adopted = song_.adoptSceneNotes(track, scenePattern(tr.scene), endBars(song_));
    tr.seqBlocks = true;
    const int i = blockAt(tr, bar);
    SeqPatternPtr notes = i >= 0 ? tr.clips[(size_t)i].notes : nullptr;
    if (notes && (notes->bars > PatternSequencer::kMaxBars || notes->notes.size() > PatternSequencer::kMaxEvents)) {
        logf(true, "seq view: T%u block on bar %u is %u bars, %u notes: more than a scene holds",
             (unsigned)track + 1, (unsigned)tr.clips[(size_t)i].bar + 1, (unsigned)notes->bars,
             (unsigned)notes->notes.size());
        if (adopted) commit();
        return false;
    }
    seq_.clearSeq(scene, 127);
    seq_.setSceneBars(scene, notes ? (uint8_t)notes->bars : PatternSequencer::kDefaultBars);
    if (notes) {
        for (const SeqNote& n : notes->notes)
            seq_.addEventTicks(scene, n.tick, n.pad, n.vel, n.len, PadPerformance::unpack(n.perf));
    }
    editTrack_ = track;
    editBar_ = i >= 0 ? tr.clips[(size_t)i].bar : bar;
    editScene_ = scene;
    editBound_ = i >= 0;
    editGen_ = seq_.editGeneration();
    /* The view plays the scene on its own clock: through the track's kit. */
    seq_.setSceneRoute((uint8_t)std::max(0, song_.seqRoute(track)));
    logf(false, "seq view: T%u bar %u, %s: %u notes into scene %u", (unsigned)track + 1,
         (unsigned)editBar_ + 1, i >= 0 ? "a block" : "no block", notes ? (unsigned)notes->notes.size() : 0u,
         (unsigned)scene);
    if (adopted) commit();
    return true;
}

/// The scene back onto the block it edits. Not while the seq view's REC
/// runs -- every hit would be a commit -- unless the view is letting go.
void SongEngine::Impl::syncSeqEditNow(bool evenRecording) {
    if (editTrack_ < 0 || seq_.editGeneration() == editGen_) return;
    if (!evenRecording && seq_.isRecording()) return;
    std::lock_guard<std::recursive_mutex> l(songMutex_);
    if (editTrack_ < 0) return;
    editGen_ = seq_.editGeneration();
    const int track = editTrack_.load();
    SongTrack& tr = song_.tracks[track];
    if (!tr.inUse || tr.kind != TrackKind::Seq) { editTrack_ = -1; return; }
    const SeqPatternPtr now = scenePattern(editScene_);
    int i = -1;
    for (size_t k = 0; k < tr.clips.size() && i < 0; ++k)
        if (tr.clips[k].bar == editBar_) i = (int)k;
    if (i < 0) {
        /* The block went (deleted, undone) while it was being edited:
         * nothing is put back in its place. */
        if (editBound_ || now->notes.empty()) {
            if (editBound_) editTrack_ = -1;
            return;
        }
        /* The first notes of a new block: on its bar, as long as the
         * scene, never over the next block. */
        SongClip c;
        c.bar = editBar_;
        c.bars = std::min<uint32_t>(now->bars, roomAt(tr, editBar_));
        c.notes = now;
        song_.addClip(track, c);
        editBound_ = true;
        commit();
        return;
    }
    SongClip& c = tr.clips[(size_t)i];
    if (samePattern(c.notes, now)) return;
    /* Notes leaving a block is what the player loses: said each time. */
    const size_t had = c.notes ? c.notes->notes.size() : 0;
    if (now->notes.size() < had) {
        logf(true, "seq view: T%u block on bar %u: %u notes -> %u", (unsigned)track + 1, (unsigned)c.bar + 1,
             (unsigned)had, (unsigned)now->notes.size());
    }
    /* A block as long as its notes stays so when the scene grows or
     * shrinks (x2, /2, its length): what the seq view shows is the block.
     * One the notes go round in several times keeps its length until x2
     * makes them longer than it is: then it grows with them, so x2 in
     * the seq view always lengthens the block in Arrange too. */
    if (c.notes && c.notes->bars) {
        const uint32_t want = std::min<uint32_t>(now->bars, roomAt(tr, c.bar));
        if (c.bars == c.notes->bars) c.bars = (uint16_t)want;
        else if (c.bars % c.notes->bars == 0 && want > c.bars) c.bars = (uint16_t)want;
    }
    c.notes = now;
    commit();
}

void SongEngine::Impl::closeSeqEdit() {
    /* Left with its REC still on -- ARR, the MODE map, another clip: the
     * take lands as far as it got. The next transport claim stops it a
     * moment later, and by then nothing is bound to write it back; it was
     * lost, and the block in Arrange kept its old notes (13.10.2026). */
    syncSeqEditNow(true);
    std::lock_guard<std::recursive_mutex> l(songMutex_);
    editTrack_ = -1;
    seq_.setSceneRoute(0);
}

bool SongEngine::Impl::seqPunchable() const {
    return play_.load() == Play::Running && syncing_.load() && seq_.state() == SequencerState::Running;
}

/// Ready the song's armed seq track for a take (armedSeqTrack()): made
/// if there is none (the kit, on the lowest free track), its blocks given
/// the scene's notes as their own, and the sequencer keeping hits. A REC
/// is not heard under itself. Caller holds songMutex_.
bool SongEngine::Impl::openTake(SyncTake take, uint32_t fromBar) {
    int t = song_.armedSeqTrack();
    const bool made = t < 0;
    if (made) {
        t = song_.freeTrack();
        /* A scene of its own, which the seq view edits the track's
         * blocks in -- not the groove up in the Sequencer. With no room
         * for another the one up is lent, as Arrange's SEQ card does. */
        if (t < 0) return false;
        const bool added = seq_.addScene();
        const int scene = song_.sceneForNewSeq(seq_.sceneCount(), added);
        if (scene < 0 || !song_.addSeqTrack(t, SeqSource::Kit, (uint8_t)scene)) return false;
        /* A song that starts as a seq track has no clip to take its
         * tempo from: its grid is the sequencer's. */
        if (song_.bpm <= 0.0f) song_.bpm = seq_.getClickBpm();
    }
    if (!seq_.beginSongTake(kTakeRoom)) return false;
    /* A track made for the take holds the take and nothing else; one
     * that played its scene keeps playing what it played. */
    if (!made) song_.adoptSceneNotes(t, scenePattern(song_.tracks[t].scene), endBars(song_));
    song_.tracks[t].seqBlocks = true;
    takeTrack_ = t;
    /* LOOP on, a take goes round: a region, or else the whole song -- one with
     * something in it already; a new song's first take sets its length. LOOP
     * off runs the song on past its end (takeHorizon_), and so does a take from a
     * bar past the end: the playhead put there is the song asked to grow. A take
     * punched in at the end of a four-bar groove ran it on to bar six instead of
     * laying the part over the groove (26.10.2026). */
    const uint32_t songEnd = endBars(song_);
    takeWhole_ = song_.loop && !song_.hasRegion() && fromBar < songEnd;
    takeCycle_ = (song_.loop && song_.hasRegion()) || takeWhole_;
    takeCycleA_ = takeWhole_ ? 0 : song_.loopStart;
    takeCycleB_ = takeWhole_ ? songEnd : song_.loopEnd;
    takeReplaced_ = false;
    takeLaps_ = 0;
    takeNotes_.clear();
    takeHorizon_ = !takeCycle_;
    takeRecMute_ = take == SyncTake::Record;
    seqTake_.store(take);
    commit();
    return true;
}

/// The block of @p tr sounding on @p bar, or -1.
int SongEngine::Impl::blockAt(const SongTrack& tr, uint32_t bar) {
    const auto& cs = tr.clips;
    for (size_t i = 0; i < cs.size(); ++i) {
        uint32_t end = cs[i].bar + cs[i].bars;
        if (i + 1 < cs.size()) end = std::min(end, cs[i + 1].bar);
        if (bar >= cs[i].bar && bar < end) return (int)i;
    }
    return -1;
}

/// Bars free on @p tr from @p bar up to its next block, at least one.
uint32_t SongEngine::Impl::roomAt(const SongTrack& tr, uint32_t bar) {
    uint32_t room = 0xFFFF;
    for (const auto& c : tr.clips) if (c.bar > bar) room = std::min(room, c.bar - bar);
    return std::max<uint32_t>(room, 1);
}

bool SongEngine::Impl::samePattern(const SeqPatternPtr& a, const SeqPatternPtr& b) {
    if (a == b) return true;
    if (!a || !b || a->bars != b->bars || a->notes.size() != b->notes.size()) return false;
    for (size_t i = 0; i < a->notes.size(); ++i) {
        const SeqNote& x = a->notes[i];
        const SeqNote& y = b->notes[i];
        if (x.tick != y.tick || x.len != y.len || x.pad != y.pad || x.vel != y.vel || x.perf != y.perf) return false;
    }
    return true;
}

/// The take is off: the sequencer keeps no more hits, the track is heard
/// again and the song ends where its blocks do. Caller holds songMutex_.
void SongEngine::Impl::closeTake() {
    seq_.endSongTake();
    takeRecMute_ = false;
    seqTake_.store(SyncTake::None);
    applySeqSound();
    takeTrack_ = -1;
    takeNotes_.clear();
    if (takeHorizon_) {
        takeHorizon_ = false;
        rebuild();
    }
}

SceneLookup SongEngine::Impl::scenes() const {
    SceneLookup l;
    l.pattern = [this](uint8_t scene) { return scenePattern(scene); };
    l.bars = [this](uint8_t scene) { return seq_.sceneBars(scene); };
    return l;
}

/// Scene @p scene's notes as a pattern of its own: what a block that
/// played the scene is given before a take changes it.
SeqPatternPtr SongEngine::Impl::scenePattern(uint8_t scene) const {
    std::vector<SeqNote> notes;
    const uint16_t n = seq_.eventCount(scene);
    notes.reserve(n);
    for (uint16_t i = 0; i < n; ++i) {
        uint32_t ticks = 0;
        uint8_t pad = 0, vel = 0;
        if (!seq_.eventAtTicks(scene, i, ticks, pad, vel)) continue;
        SeqNote o;
        o.tick = ticks;
        o.len = seq_.eventLengthTicks(scene, i);
        o.pad = pad;
        o.vel = vel;
        o.perf = seq_.eventPerformance(scene, i).pack();
        notes.push_back(o);
    }
    const uint8_t bars = seq_.sceneBars(scene);
    return makeSeqPattern(bars ? bars : 1, std::move(notes));
}

/// Worker, while it streams: the hits taken so far are moved out of the
/// sequencer, and a lap of the region that is over lands.
void SongEngine::Impl::serviceTake() {
    if (seqTake_.load() == SyncTake::None) return;
    std::lock_guard<std::recursive_mutex> l(songMutex_);
    if (seqTake_.load() == SyncTake::None) return;
    seq_.takeSongNotes(takeNotes_, false);
    const uint16_t laps = seq_.songTakeLaps();
    if (takeCycle_ && laps != takeLaps_) landLaps(laps);
}

/// Every lap before @p upto lands, one pass each: a REC's first pass with
/// notes replaces the region, every pass after it is dubbed in -- and
/// once the first has landed the track is heard, so the next lap plays
/// along with what was just played. Caller holds songMutex_.
void SongEngine::Impl::landLaps(uint16_t upto) {
    const uint32_t spb = song_.samplesPerBar(sampleRate_);
    for (; takeLaps_ != upto; ++takeLaps_) {
        std::vector<PatternSequencer::SongTakeNote> pass, rest;
        for (const auto& n : takeNotes_) (n.lap == takeLaps_ ? pass : rest).push_back(n);
        takeNotes_.swap(rest);
        const bool replace = seqTake_.load() == SyncTake::Record && !takeReplaced_;
        const uint32_t from = takeLaps_ == 0 ? takeFrom_ : takeCycleA_ * spb;
        /* A region's first pass replaces the region; round the whole song, to the
         * bar of its last note, as a take without a cycle does. */
        const uint32_t to = takeWhole_ ? from : takeCycleB_ * spb;
        if (landTake(pass, replace, from, to) && replace) {
            takeReplaced_ = true;
            takeRecMute_ = false;
            applySeqSound();
        }
    }
}

/// A stretch of the take onto the track: REC as blocks from the bar
/// @p from is in to the bar @p to ends in, or past its last note if that
/// is later -- @p to == @p from is "to the bar of the last note" -- the seq
/// blocks under it gone; DUB into the blocks it fell under. Song samples.
/// Nothing played, nothing changes. Caller holds songMutex_.
bool SongEngine::Impl::landTake(const std::vector<PatternSequencer::SongTakeNote>& notes, bool replace,
                                uint32_t from, uint32_t to) {
    if (notes.empty() || takeTrack_ < 0) return false;
    const uint32_t spb = song_.samplesPerBar(sampleRate_);
    const uint32_t bpb = song_.beatsPerBar ? song_.beatsPerBar : AudioSong::kDefaultBeatsPerBar;
    const uint32_t beat = spb / bpb ? spb / bpb : 1;
    const uint32_t tpb = song_.seqTicksPerBar();
    auto ticks = [&](uint32_t frames) {
        return (uint32_t)(((uint64_t)frames * AudioSong::kSeqPpq + beat / 2) / beat);
    };
    if (!replace) {
        std::vector<SeqNote> dub;
        dub.reserve(notes.size());
        for (const auto& n : notes) dub.push_back({ticks(n.pos), ticks(n.len), n.pad, n.vel, n.perf});
        song_.dubSeqNotes(takeTrack_, dub);
    } else {
        const uint32_t fromBar = from / spb;
        uint32_t last = fromBar + 1;   // the bar after the last note's
        for (const auto& n : notes) last = std::max<uint32_t>(last, n.pos / spb + 1);
        const uint32_t toBar = std::max<uint32_t>(last, (to + spb - 1) / spb);
        std::vector<SeqNote> mine;
        mine.reserve(notes.size());
        for (const auto& n : notes) {
            const uint32_t t = ticks(n.pos);
            if (t < fromBar * tpb) continue;
            mine.push_back({t - fromBar * tpb, ticks(n.len), n.pad, n.vel, n.perf});
        }
        /* Every hit before the bar it started on (in the count-in): nothing
         * was played, and that changes nothing -- not an empty block over
         * what was there. */
        if (mine.empty()) return false;
        /* Shorter than the loop it was played in, it goes round to the
         * loop's end: one bar played into four is that bar four times,
         * not a bar and then silence while the loop goes on (20.10.2026).
         * The pattern is the shortest that both holds the notes and
         * divides the loop, so it lands on its bar one every time round. */
        const uint32_t fill = takeFillEnd(fromBar, last);
        uint32_t pattern = 0;
        if (fill > fromBar && mine.size() <= AudioSong::kSeqBlockNotes) {
            const uint32_t room = fill - fromBar;
            const uint32_t most = std::min<uint32_t>(room, AudioSong::kSeqBlockBars);
            for (uint32_t d = last - fromBar; d <= most && !pattern; ++d)
                if (room % d == 0) pattern = d;
        }
        if (pattern) {
            song_.putSeqTake(takeTrack_, fromBar, makeSeqPattern((uint16_t)pattern, std::move(mine)), spb,
                             fill - fromBar);
        } else {
            song_.putSeqTakeNotes(takeTrack_, fromBar, std::min<uint32_t>(toBar - fromBar, 0xFFFF),
                                  std::move(mine), spb);
        }
    }
    commit();
    return true;
}

/// Where a REC's block from @p fromBar, its notes ending on bar @p last,
/// goes round to: the region it was taken round, or -- the song looping
/// as a whole -- the song's end, but only over bars where the track has
/// nothing else sounding. 0 = it ends with its notes. Caller holds
/// songMutex_.
uint32_t SongEngine::Impl::takeFillEnd(uint32_t fromBar, uint32_t last) const {
    if (takeCycle_ && !takeWhole_) return last <= takeCycleB_ ? takeCycleB_ : 0;
    if (!song_.loop || takeTrack_ < 0) return 0;
    uint32_t end = endBars(song_);
    const auto& cs = song_.tracks[takeTrack_].clips;
    for (size_t i = 0; i < cs.size(); ++i) {
        uint32_t e = cs[i].bar + cs[i].bars;
        if (i + 1 < cs.size()) e = std::min(e, cs[i + 1].bar);
        if (e > last) end = std::min(end, std::max(cs[i].bar, last));
    }
    return end > last ? end : 0;
}

/// The take is over -- stopped, or punched out with the song playing on:
/// what is left of it lands. A REC ends with the bar its last note is in,
/// wherever STOP was pressed: a note on the last quarter of bar four is a
/// four-bar sequence, not five because the stop came in bar five
/// (13.10.2026; it ran to the stop before). A lap round a region is the
/// region's, and is landed whole as it wraps (landLaps()). Caller holds
/// songMutex_.
void SongEngine::Impl::finishTake() {
    const SyncTake take = seqTake_.load();
    if (take == SyncTake::None) return;
    seq_.takeSongNotes(takeNotes_, true);
    const uint32_t spb = song_.samplesPerBar(sampleRate_);
    if (takeCycle_) {
        const uint16_t laps = seq_.songTakeLaps();
        if (laps != takeLaps_) landLaps(laps);
    }
    const uint32_t from = takeCycle_ && takeLaps_ ? takeCycleA_ * spb : takeFrom_;
    landTake(takeNotes_, take == SyncTake::Record && !takeReplaced_, from, from);
    closeTake();
}

} // namespace crosspad

// SPDX-License-Identifier: GPL-3.0-or-later
//
// SongEngine, the edits: tempo, merge, import, Live Set export, undo and redo.
// The state is SongEngine::Impl (SongEngineImpl.hpp); the worker loop is in SongEngine.cpp.

#include "SongEngineImpl.hpp"

namespace crosspad {

bool SongEngine::Impl::requestTempo(float bpm) {
    if (!(bpm >= kMinBpm && bpm <= kMaxBpm) || !ensureWorker() || !claimEdit()) return false;
    editBpm_.store(bpm);
    return requestEdit(kEditTempo);
}

bool SongEngine::Impl::requestMerge(uint8_t trackMask, uint32_t fromBar, uint32_t toBar) {
    const uint8_t all = (uint8_t)((1u << kTracks) - 1);
    if (!(trackMask & all) || toBar <= fromBar || !ensureWorker() || !claimEdit()) return false;
    editMask_.store(trackMask & all);
    editFrom_.store(fromBar);
    editTo_.store(toBar);
    editProgress_.store(0);
    return requestEdit(kEditMerge);
}

bool SongEngine::Impl::requestExportLiveSet() {
    if (!ensureWorker() || !claimEdit()) return false;
    return requestEdit(kEditExport);
}

std::vector<std::string> SongEngine::Impl::editReport() {
    std::lock_guard<std::mutex> l(reportMutex_);
    return report_;
}

/// Where audio meant for @p track may go (UX 2000 section 7.1): that
/// track if it is an audio track, the next free one if nobody has made
/// it yet -- tracks are made from the lowest free one, never with a hole
/// below -- and nowhere (-1) if it is the seq track or all four are taken.
/// @p make: also make the free one. Caller holds songMutex_.
int SongEngine::Impl::audioTarget(uint8_t track, bool make) {
    if (track >= kTracks) return -1;
    SongTrack& t = song_.tracks[track];
    /* A track holding clips is in use whatever its flag says: songs from
     * before tracks had kinds are exactly that. */
    if (t.inUse || !t.clips.empty()) {
        if (t.kind != TrackKind::Audio) return -1;
        if (make) t.inUse = true;
        return track;
    }
    const int f = song_.freeTrack();
    if (f >= 0 && make) song_.addAudioTrack(f);
    return f;
}

bool SongEngine::Impl::requestImport(const std::string& file, uint32_t start, uint32_t end,
                                     uint8_t track, uint32_t bar, uint32_t lead) {
    if (file.empty() || track >= kTracks || (end && end <= start) || !ensureWorker()) return false;
    /* Checked before the claim: a refusal after it would leave the edit
     * Busy for good. */
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (audioTarget(track, false) < 0) return false;
    }
    if (!claimEdit()) return false;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        editName_ = file;
    }
    importA_ = start;
    importB_ = end;
    importTrack_ = track;
    importBar_ = bar;
    importLead_ = lead;
    editProgress_.store(0);
    return requestEdit(kEditImport);
}

/// Worker: a new tempo, and every block as long as its file lasts at it.
/// The headers are read into a copy outside songMutex_, so the UI's
/// timers are not held out by the card.
SongOpState SongEngine::Impl::doTempo(float bpm) {
    if (!opened_) return SongOpState::Failed;
    AudioSong copy;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        copy = song_;
    }
    copy.bpm = bpm;
    const uint32_t spBar = copy.samplesPerBar(sampleRate_);
    for (auto& tr : copy.tracks) {
        /* A stretched track keeps its blocks' bars: its audio is fitted
         * to them (doStretch), not they to it. */
        if (tr.stretch != StretchMode::Off && tr.kind == TrackKind::Audio) continue;
        for (auto& c : tr.clips) {
            /* A seq block is bars of the scene: it keeps its length. */
            if (c.file.empty()) continue;
            ClipReader r;
            if (!p.storageReady() || !r.open(copy.clipPath(c.file))) continue;   // missing: keep what it had
            /* What the block plays: from its offset, to a cut or the end. */
            const uint32_t rest = r.frames > c.offset ? r.frames - c.offset : 0;
            const uint32_t len = c.frames ? std::min(c.frames, rest) : rest;
            c.bars = (uint16_t)std::max<uint32_t>(1, std::min<uint32_t>((len + spBar - 1) / spBar, 999));
            r.close();
            p.yieldIo();
        }
    }
    endPlayback();
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        song_.bpm = bpm;
        /* Sizes matched to the blocks by place: an edit made while the
         * headers were read would have moved them, and wins. */
        for (int t = 0; t < kTracks; ++t) {
            auto& cs = song_.tracks[t].clips;
            const auto& nc = copy.tracks[t].clips;
            for (size_t i = 0; i < cs.size() && i < nc.size(); ++i) {
                if (cs[i].file == nc[i].file && cs[i].bar == nc[i].bar && cs[i].offset == nc[i].offset)
                    cs[i].bars = nc[i].bars;
            }
        }
        rebuild();
    }
    doSave();
    return SongOpState::Ready;
}

/// Worker: the tracks in @p mask mixed down, [fromBar, toBar) and on to the
/// end of any block that starts inside, into one clip on the lowest of
/// them -- see ISongEngine::requestMerge. Read from a copy of the song,
/// so the UI is not held out while the card works; the song is changed
/// only once the clip is written.
SongOpState SongEngine::Impl::doMerge(uint8_t mask, uint32_t fromBar, uint32_t toBar) {
    if (!opened_ || !p.storageReady() || !io_ || !acc_) return SongOpState::Failed;
    AudioSong copy;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        copy = song_;
    }
    /* Muted or silent tracks are not what is heard: left out, left alone.
     * A seq track has no audio to mix until it is bounced: likewise. */
    for (int t = 0; t < kTracks; ++t) {
        if (copy.tracks[t].muted || copy.tracks[t].volume <= 0.0f ||
            copy.tracks[t].kind == TrackKind::Seq) mask &= (uint8_t)~(1u << t);
    }
    if (!mask) return SongOpState::Failed;
    for (int t = 0; t < kTracks; ++t) {
        if (!(mask & (1u << t))) continue;
        for (const auto& c : copy.tracks[t].clips) {
            if (c.bar >= fromBar && c.bar < toBar) toBar = std::max<uint32_t>(toBar, c.bar + c.bars);
        }
    }
    int target = 0;
    while (!(mask & (1u << target))) ++target;
    /* Blocks the merge leaves on the target track keep playing at its
     * volume, so the clip is mixed relative to it; with none left the
     * volumes go into the clip and the track goes to unity. */
    bool targetKeepsBlocks = false;
    for (const auto& c : copy.tracks[target].clips) {
        if (c.bar < fromBar || c.bar >= toBar) targetKeepsBlocks = true;
    }
    const float refVolume = targetKeepsBlocks ? copy.tracks[target].volume : 1.0f;
    const Arrangement arr = flatten(copy);
    const uint32_t spBar = arr.samplesPerBar;
    const uint32_t a = fromBar * spBar, b = toBar * spBar;
    std::string file;
    for (int n = 1; n < 1000 && file.empty(); ++n) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "merge_%02d.wav", n);
        if (!fsx::exists(copy.clipPath(buf))) file = buf;
    }
    if (file.empty()) return SongOpState::Failed;
    const std::string path = copy.clipPath(file);

    endPlayback();
    WavWriter w;
    if (!w.open(path.c_str(), sampleRate_, 2)) return SongOpState::Failed;
    w.reserve(b - a);                           // the card's clusters found now, not per chunk
    ClipReader rd[kTracks];
    bool ok = true;
    for (uint32_t f = a; f < b && ok;) {
        const uint32_t n = std::min(sz.chunkFrames, b - f);
        std::memset(acc_, 0, n * 8);
        for (int t = 0; t < kTracks; ++t) {
            if (!(mask & (1u << t))) continue;
            const float g = copy.tracks[t].volume / refVolume;
            /* Each track's pan goes into the clip, but the target's own
             * while it keeps blocks: that one plays on at the track. */
            const float pan = t == target && targetKeepsBlocks ? 0.0f : copy.tracks[t].pan;
            const float gs[2] = {g * (pan > 0.0f ? 1.0f - pan : 1.0f), g * (pan < 0.0f ? 1.0f + pan : 1.0f)};
            for (const auto& sg : arr.tracks[t]) {
                if (sg.end <= f) continue;
                if (sg.start >= f + n) break;
                const uint32_t x = std::max(sg.start, f), y = std::min(sg.end, f + n);
                if (rd[t].path != sg.path && !(p.storageReady() && rd[t].open(sg.path))) continue;
                rd[t].read(x - sg.start + sg.offset, io_, y - x);
                int32_t* d = acc_ + (x - f) * 2;
                for (uint32_t k = 0; k < (y - x) * 2; ++k) d[k] += (int32_t)((float)io_[k] * gs[k & 1]);
            }
        }
        for (uint32_t k = 0; k < n * 2; ++k) {
            const int32_t v = acc_[k];
            io_[k] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
        ok = w.write(io_, n);
        f += n;
        editProgress_.store((uint8_t)((uint64_t)(f - a) * 99 / (b - a)));
        p.yieldIo();
    }
    for (auto& r : rd) r.close();
    if (!w.close() || !ok) {
        std::remove(path.c_str());
        return SongOpState::Failed;
    }

    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        for (int t = 0; t < kTracks; ++t) {
            if (!(mask & (1u << t))) continue;
            auto& cs = song_.tracks[t].clips;
            cs.erase(std::remove_if(cs.begin(), cs.end(), [&](const SongClip& c) {
                return c.bar >= fromBar && c.bar < toBar;
            }), cs.end());
            /* A block reaching into the range from before it keeps what
             * is before it; the rest is in the new clip. */
            for (auto& c : cs) {
                if (c.bar < fromBar && c.bar + c.bars > fromBar) c.bars = (uint16_t)(fromBar - c.bar);
            }
        }
        SongClip c;
        c.file = file;
        c.bar = fromBar;
        c.bars = (uint16_t)std::max<uint32_t>(1, toBar - fromBar);
        song_.addClip(target, c);
        if (!targetKeepsBlocks) {
            song_.tracks[target].volume = 1.0f;
            song_.tracks[target].pan = 0.0f;
        }
        rebuild();
    }
    doSave();
    editProgress_.store(100);
    logf(false, "merged tracks 0x%x, bars %u..%u, into %s", (unsigned)mask, (unsigned)fromBar,
         (unsigned)toBar, file.c_str());
    return SongOpState::Ready;
}

/// Worker: the open song as a Live Set in its own folder -- see
/// ISongEngine::requestExportLiveSet. The song is copied, the clips'
/// headers read, and the Set written to a .tmp that only replaces the
/// .als once it is whole.
SongOpState SongEngine::Impl::doExport() {
    AudioSong copy;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        copy = song_;
    }
    std::vector<std::string> lines;
    auto finish = [&](SongOpState st) {
        for (const auto& ln : lines) logf(st == SongOpState::Failed, "export: %s", ln.c_str());
        std::lock_guard<std::mutex> l(reportMutex_);
        report_ = std::move(lines);
        return st;
    };
    if (!opened_ || copy.dir.empty() || !p.storageReady()) {
        lines.push_back("No song open, or no card.");
        return finish(SongOpState::Failed);
    }
    endPlayback();

    /* Each file once; one not on the card stays off the list, which is
     * how the report tells missing from unreadable. */
    std::vector<LiveSetClipFile> files;
    std::vector<std::string> seen;
    for (const auto& t : copy.tracks) {
        for (const auto& c : t.clips) {
            if (std::find(seen.begin(), seen.end(), c.file) != seen.end()) continue;
            seen.push_back(c.file);
            LiveSetClipFile f;
            f.file = c.file;
            const std::string path = copy.clipPath(c.file);
            if (!fsx::fileInfo(path, f.bytes, f.modified)) continue;
            WavInfo info;
            if (wavReadInfo(path.c_str(), info)) {
                f.frames = info.frames;
                f.sampleRate = info.sampleRate;
            }
            files.push_back(f);
            p.yieldIo();
        }
    }

    std::string skeleton = p.liveSetTemplate();
    if (skeleton.empty()) {
        lines.push_back("Not enough memory to build the Set (" +
                        std::to_string(p.freeMemoryBytes() / 1024) + " kB free).");
        return finish(SongOpState::Failed);
    }
    /* An empty Project12.cfg makes the folder a Live project, and that is
     * what the clips' paths are relative to; Live fills it in on its
     * first save. Without it the Set opens with every file missing. */
    const std::string projectInfo = copy.dir + "/Ableton Project Info";
    const std::string cfg = projectInfo + "/Project12.cfg";
    if (!fsx::exists(cfg)) {
        std::FILE* f = fsx::makeDir(projectInfo) ? std::fopen(cfg.c_str(), "wb") : nullptr;
        if (!f) {
            lines.push_back("The card would not take the project's folder.");
            return finish(SongOpState::Failed);
        }
        std::fclose(f);
    }

    const std::string als = copy.dir + "/" + copy.name + ".als";
    const std::string tmp = als + ".tmp";
    GzipStoredWriter gz;
    LiveSetReport rep;
    bool ok = gz.open(tmp) &&
              writeLiveSetXml(std::move(skeleton), copy, files,
                              [&](const char* d, size_t n) { p.yieldIo(); return gz.write(d, n); }, rep);
    ok = gz.close() && ok;
    if (!ok) {
        std::remove(tmp.c_str());
        lines.push_back("The Set was not written: the card is full or would not take it.");
        return finish(SongOpState::Failed);
    }
    /* One Live saved over is not lost: FatFs will not rename onto a file,
     * and the one there goes to Backup/ rather than away. */
    std::string kept;
    if (fsx::exists(als)) {
        const std::string backup = copy.dir + "/Backup";
        for (int n = 1; n < 1000 && kept.empty(); ++n) {
            const std::string name = copy.name + " [" + std::to_string(n) + "].als";
            if (!fsx::exists(backup + "/" + name)) kept = name;
        }
        if (kept.empty() || !fsx::makeDir(backup) || !fsx::rename(als, backup + "/" + kept)) {
            std::remove(tmp.c_str());
            lines.push_back("The Set already there could not be moved to Backup/; nothing was changed.");
            return finish(SongOpState::Failed);
        }
    }
    if (!fsx::rename(tmp, als)) {
        lines.push_back("The Set was written but could not be named " + copy.name + ".als.");
        return finish(SongOpState::Failed);
    }
    std::string root = p.songsRoot();
    root = root.substr(root.find_last_of('/') + 1);
    lines.push_back(root + "/" + copy.name + "/" + copy.name + ".als, " + std::to_string(rep.clips) +
                    (rep.clips == 1 ? " clip" : " clips"));
    if (!kept.empty()) lines.push_back("The one there before is in Backup/ as " + kept);
    lines.insert(lines.end(), rep.notes.begin(), rep.notes.end());
    return finish(SongOpState::Ready);
}

/// Worker: a region of a WAV into the project's clips/, laid on a track
/// -- see ISongEngine::requestImport. The clip is named after its source,
/// numbered when the project has one of that name already.
SongOpState SongEngine::Impl::doImport() {
    std::string src;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        src = editName_;
    }
    if (!opened_ || !io_ || !p.storageReady()) return SongOpState::Failed;
    ClipReader r;
    if (!r.open(src)) return SongOpState::Failed;
    uint32_t a = importA_, b = importB_;
    if (!b || b > r.frames) b = r.frames;
    if (a >= b) { r.close(); return SongOpState::Failed; }
    std::string stem = src.substr(src.find_last_of("/\\") + 1);
    if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".wav") == 0) stem.resize(stem.size() - 4);
    if (stem.size() > kMaxClipPrefix) stem.resize(kMaxClipPrefix);
    std::string file, path;
    for (int n = 1; n < 100 && file.empty(); ++n) {
        const std::string cand = n == 1 ? stem + ".wav" : stem + "_" + std::to_string(n) + ".wav";
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (!fsx::exists(song_.clipPath(cand))) { file = cand; path = song_.clipPath(cand); }
    }
    if (file.empty()) { r.close(); return SongOpState::Failed; }

    endPlayback();                              // the worker cannot keep the ring filled meanwhile
    const uint32_t lead = importLead_, total = lead + (b - a);
    WavWriter w;
    if (!w.open(path.c_str(), sampleRate_, 2)) { r.close(); return SongOpState::Failed; }
    w.reserve(total);
    bool ok = true;
    uint32_t done = 0;
    std::memset(io_, 0, sz.chunkFrames * 4);
    while (ok && done < lead) {
        const uint32_t n = std::min(sz.chunkFrames, lead - done);
        ok = w.write(io_, n);
        done += n;
    }
    for (uint32_t f = a; ok && f < b;) {
        const uint32_t n = std::min(sz.chunkFrames, b - f);
        r.read(f, io_, n);
        ok = w.write(io_, n);
        f += n;
        done += n;
        editProgress_.store((uint8_t)((uint64_t)done * 99 / total));
        p.yieldIo();
    }
    r.close();
    if (!w.close() || !ok) {
        std::remove(path.c_str());
        return SongOpState::Failed;
    }

    const uint32_t bar = importBar_;
    uint8_t t = importTrack_;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        const int target = audioTarget(importTrack_, true);
        if (target < 0) {
            std::remove(path.c_str());
            return SongOpState::Failed;
        }
        t = (uint8_t)target;
        const uint32_t spBar = song_.samplesPerBar(sampleRate_);
        const uint32_t bars = std::max<uint32_t>(1, std::min<uint32_t>((total + spBar - 1) / spBar, 999));
        auto& cs = song_.tracks[t].clips;
        cs.erase(std::remove_if(cs.begin(), cs.end(), [&](const SongClip& c) {
            return c.bar >= bar && c.bar < bar + bars;
        }), cs.end());
        for (auto& c : cs) {
            if (c.bar < bar && c.bar + c.bars > bar) c.bars = (uint16_t)(bar - c.bar);
        }
        SongClip c;
        c.file = file;
        c.bar = bar;
        c.bars = (uint16_t)bars;
        song_.addClip(t, c);
        rebuild();
    }
    doSave();
    editProgress_.store(100);
    logf(false, "imported %s [%u, %u) +%u to track %u bar %u as %s", src.c_str(), (unsigned)a,
         (unsigned)b, (unsigned)lead, (unsigned)(t + 1), (unsigned)bar, file.c_str());
    return SongOpState::Ready;
}

/// The arrangement as bytes -- tempo, metre, loop, length and the four
/// tracks with their blocks; not the project's name or folder, and not
/// the launcher's sets. One allocation, so a step costs PSRAM rather than
/// internal RAM for every block's name; and equal arrangements are equal
/// bytes, which is how a change is told from a commit that changed nothing.
void SongEngine::Impl::pack(const AudioSong& s, Snapshot& out) {
    out.bytes.clear();
    out.notes.clear();
    out.bytes.reserve(512);
    auto put = [&](const void* v, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(v);
        out.bytes.insert(out.bytes.end(), b, b + n);
    };
    auto str = [&](const std::string& v) {
        const uint16_t n = (uint16_t)std::min<size_t>(v.size(), 0xFFFF);
        put(&n, 2);
        put(v.data(), n);
    };
    const uint8_t flags = (s.loop ? 1 : 0) | (s.syncSeq ? 2 : 0);
    put(&s.bpm, sizeof s.bpm);
    put(&s.beatsPerBar, 1);
    put(&flags, 1);
    put(&s.length, 4);
    put(&s.loopStart, 4);
    put(&s.loopEnd, 4);
    for (const auto& t : s.tracks) {
        const uint8_t head[5] = {
            (uint8_t)((t.inUse ? 1 : 0) | (t.muted ? 2 : 0) | (t.solo ? 4 : 0) | (t.seqBlocks ? 8 : 0)),
            (uint8_t)t.kind, (uint8_t)t.source, (uint8_t)t.stretch, t.scene};
        put(head, sizeof head);
        put(&t.volume, sizeof t.volume);
        put(&t.pan, sizeof t.pan);
        str(t.name);
        str(t.kit);
        const uint16_t n = (uint16_t)t.clips.size();
        put(&n, 2);
        for (const auto& c : t.clips) {
            str(c.file);
            put(&c.bar, 4);
            put(&c.bars, 2);
            put(&c.offset, 4);
            put(&c.frames, 4);
            put(&c.from, 2);
            uint16_t ni = 0xFFFF;
            if (c.notes) {
                const auto it = std::find(out.notes.begin(), out.notes.end(), c.notes);
                ni = (uint16_t)(it - out.notes.begin());
                if (it == out.notes.end()) out.notes.push_back(c.notes);
            }
            put(&ni, 2);
        }
    }
}

/// pack() undone, into @p s: its name, folder and sets are left alone.
void SongEngine::Impl::unpack(const Snapshot& in, AudioSong& s) {
    size_t at = 0;
    auto get = [&](void* v, size_t n) { std::memcpy(v, in.bytes.data() + at, n); at += n; };
    auto str = [&](std::string& v) {
        uint16_t n = 0;
        get(&n, 2);
        v.assign(reinterpret_cast<const char*>(in.bytes.data()) + at, n);
        at += n;
    };
    uint8_t flags = 0;
    get(&s.bpm, sizeof s.bpm);
    get(&s.beatsPerBar, 1);
    get(&flags, 1);
    s.loop = flags & 1;
    s.syncSeq = flags & 2;
    get(&s.length, 4);
    get(&s.loopStart, 4);
    get(&s.loopEnd, 4);
    for (auto& t : s.tracks) {
        uint8_t head[5] = {};
        get(head, sizeof head);
        t.inUse = head[0] & 1;
        t.muted = head[0] & 2;
        t.solo = head[0] & 4;
        t.seqBlocks = head[0] & 8;
        t.kind = (TrackKind)head[1];
        t.source = (SeqSource)head[2];
        t.stretch = (StretchMode)head[3];
        t.scene = head[4];
        get(&t.volume, sizeof t.volume);
        get(&t.pan, sizeof t.pan);
        str(t.name);
        str(t.kit);
        uint16_t n = 0;
        get(&n, 2);
        t.clips.assign(n, SongClip{});
        for (auto& c : t.clips) {
            str(c.file);
            get(&c.bar, 4);
            get(&c.bars, 2);
            get(&c.offset, 4);
            get(&c.frames, 4);
            get(&c.from, 2);
            uint16_t ni = 0xFFFF;
            get(&ni, 2);
            c.notes = ni < in.notes.size() ? in.notes[ni] : nullptr;
        }
    }
}

/// rebuild()'s first step, under songMutex_: if the arrangement moved
/// since it was last seen, what it was becomes an undo step -- one per
/// burst, so a fader pulled or a block nudged detent by detent goes back
/// at once -- and what could be redone is gone.
void SongEngine::Impl::noteChange() {
    Snapshot now;
    pack(song_, now);
    if (now == last_) return;
    const int64_t t = p.nowUs();
    if (!last_.empty() && (undo_.empty() || t - lastChangeUs_ > kUndoMergeUs)) {
        undo_.push_back(std::move(last_));
        if (undo_.size() > kUndoDepth) undo_.erase(undo_.begin());
    }
    redo_.clear();
    last_ = std::move(now);
    lastChangeUs_ = t;
    undoCount_.store((uint32_t)undo_.size());
    redoCount_.store(0);
}

/// A job that took a copy of the song to write back: a step now would
/// be written over when it lands.
bool SongEngine::Impl::songJobRunning() {
    return editReq_.load() != 0 || editState_.load() == SongOpState::Busy || openReq_.load() ||
           bounceReq_.load() || bounceStatus().state == BounceState::Running ||
           seqTake_.load() != SyncTake::None;
}

/// Back (or forward again) one step: what is there now goes on the other
/// stack. Only the arrangement moves -- the audio on the card is never
/// touched, so a block undone back in finds its file where it was.
bool SongEngine::Impl::step(std::vector<Snapshot>& from, std::vector<Snapshot>& to) {
    if (songJobRunning()) return false;
    float was = 0.0f, now = 0.0f;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (!opened_ || from.empty()) return false;
        Snapshot cur;
        pack(song_, cur);
        to.push_back(std::move(cur));
        if (to.size() > kUndoDepth) to.erase(to.begin());
        was = song_.bpm;
        unpack(from.back(), song_);
        last_ = std::move(from.back());
        from.pop_back();
        lastChangeUs_ = 0;           // the next edit is a step of its own
        now = song_.bpm;
        undoCount_.store((uint32_t)undo_.size());
        redoCount_.store((uint32_t)redo_.size());
        rebuild();
    }
    /* A tempo undone is a tempo change: the song stops, as doTempo
     * stops it, and the clock follows. */
    if (std::fabs(now - was) > 0.01f) {
        stop();
        if (now > 0.0f) seq_.setClickBpm(now);
    }
    saveAtUs_.store(p.nowUs() + (int64_t)kSaveDelayMs * 1000);
    saveReq_.store(true);
    p.wake();
    return true;
}

} // namespace crosspad

// SPDX-License-Identifier: GPL-3.0-or-later
//
// SongEngine, the projects: open, rename, delete, save, the lists of projects and clips, waveform peaks.
// The state is SongEngine::Impl (SongEngineImpl.hpp); the worker loop is in SongEngine.cpp.

#include "SongEngineImpl.hpp"

namespace crosspad {

std::vector<std::string> SongEngine::Impl::projects() {
    std::lock_guard<std::mutex> l(listMutex_);
    return projects_;
}

bool SongEngine::Impl::requestOpen(const std::string& name) {
    return valid_name(name) && askToOpen(name, true);
}

/// An element is a project under the songs root like any other, one level
/// further down, and is never the one a boot opens: the song it was reached
/// from is, so a restart inside an element loses nothing.
bool SongEngine::Impl::requestOpenElement(const std::string& ref) {
    return is_element_ref(ref) && askToOpen(ref, false);
}

bool SongEngine::Impl::askToOpen(const std::string& name, bool remember) {
    if (!ensureWorker()) return false;
    yieldFreeze();          // or the open waits out the whole freeze
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        openName_ = name;
        openRemember_ = remember;
    }
    openState_.store(SongOpState::Busy);
    openReq_.store(true);
    p.wake();
    return true;
}

bool SongEngine::Impl::requestRename(const std::string& name) {
    if (!valid_name(name) || !ensureWorker() || !claimEdit()) return false;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        editName_ = name;
    }
    return requestEdit(kEditRename);
}

bool SongEngine::Impl::requestDelete() {
    if (!ensureWorker() || !claimEdit()) return false;
    return requestEdit(kEditDelete);
}

bool SongEngine::Impl::clipPeaks(const std::string& file, ClipPeaks& out) {
    std::string dir;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        dir = song_.dir;
        /* An element's bake is no clip of the song: Arrange draws it no waveform. */
        for (const auto& tr : song_.tracks)
            for (const auto& c : tr.clips)
                if (c.isElement() && c.file == file) return false;
    }
    if (dir.empty() || file.empty()) return false;
    {
        std::lock_guard<std::mutex> l(peaksMutex_);
        if (dir != peaksDir_) {           // another project: none of these are its
            peaks_.clear();
            peaksWanted_.clear();
            peaksMissing_.clear();
            peaksDir_ = dir;
        }
        auto it = peaks_.find(file);
        if (it != peaks_.end()) {
            out.hop = kPeakHop;
            out.values = it->second;
            out.missing = peaksMissing_.count(file) != 0;
            return true;
        }
        if (std::find(peaksWanted_.begin(), peaksWanted_.end(), file) != peaksWanted_.end()) return false;
        peaksWanted_.push_back(file);
    }
    if (ensureWorker()) p.wake();
    return false;
}

/// Why a playing song can be silent: the published gains the worker
/// gates on, how many segments flatten() gave each track, where the
/// write head is against the end, and what the ring holds; the context
/// stem's segments, whether its file is open and whether SOLO is on.
const char* SongEngine::Impl::debugLine() {
    uint32_t end = 0, la = 0, lb = 0;
    int seg[kTracks] = {};
    int ctxSeg = 0;
    {
        std::lock_guard<std::mutex> l(snapMutex_);
        end = snap_.end; la = snap_.loopA; lb = snap_.loopB;
        for (int t = 0; t < kTracks; ++t) seg[t] = (int)snap_.tracks[t].size();
        ctxSeg = (int)snap_.context.size();
    }
    /* The stream's margins since the last read, read and cleared: the
     * longest the worker was off the ring, its longest card access and
     * song.json write, and the least the ring held. */
    const uint32_t ringMin = dbgRingMin_.exchange(UINT32_MAX);
    std::snprintf(dbg_, sizeof(dbg_),
                  "g=%.2f/%.2f/%.2f/%.2f seg=%d/%d/%d/%d end=%u loop=%u-%u "
                  "wp=%u ringfill=%u ringcap=%u ringmin=%u gap_max_us=%u read_max_us=%u "
                  "save_max_us=%u play=%d job=%d live=%d ctx=%d/%d/%d",
                  (double)gain_[0].load(), (double)gain_[1].load(),
                  (double)gain_[2].load(), (double)gain_[3].load(),
                  seg[0], seg[1], seg[2], seg[3],
                  (unsigned)end, (unsigned)la, (unsigned)lb,
                  (unsigned)writePos_, (unsigned)ring_.used(), (unsigned)ring_.cap,
                  ringMin == UINT32_MAX ? 0u : (unsigned)ringMin,
                  (unsigned)dbgGapMaxUs_.exchange(0), (unsigned)dbgReadMaxUs_.exchange(0),
                  (unsigned)dbgSaveMaxUs_.exchange(0),
                  (int)play_.load(), (int)job_.load(), (int)liveOn_.load(),
                  ctxSeg, (int)(ctxReader_.fd >= 0), (int)ctxSolo_.load());
    return dbg_;
}

void SongEngine::Impl::dbgMax(std::atomic<uint32_t>& a, int64_t v) {
    const uint32_t u = v > 0 ? (uint32_t)std::min<int64_t>(v, UINT32_MAX) : 0;
    if (u > a.load(std::memory_order_relaxed)) a.store(u, std::memory_order_relaxed);
}

/// The first look at the song is what opens the last project.
AudioSong& SongEngine::Impl::song() {
    if (!opened_ && ensureWorker()) p.wake();
    return song_;
}

void SongEngine::Impl::commit() {
    if (!ensureWorker()) return;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        rebuild();
    }
    saveAtUs_.store(p.nowUs() + (int64_t)kSaveDelayMs * 1000);
    saveReq_.store(true);
    p.wake();
}

std::vector<ClipInfo> SongEngine::Impl::clips() {
    std::lock_guard<std::mutex> l(listMutex_);
    return clips_;
}

std::vector<ElementInfo> SongEngine::Impl::elements() {
    std::lock_guard<std::mutex> l(listMutex_);
    return elements_;
}

/// Worker: project folders under the songs root.
void SongEngine::Impl::doListProjects() {
    std::vector<std::string> out;
    if (p.storageReady()) {
        const std::string root = p.songsRoot();
        fsx::listDir(root, [&](const std::string& name, bool isDir) {
            if (name.empty() || name[0] == '.') return;
            if (isDir) out.push_back(name);
            p.yieldIo();
        });
    }
    std::sort(out.begin(), out.end());
    std::lock_guard<std::mutex> l(listMutex_);
    projects_ = std::move(out);
}

/// Worker: the clips of the open project, each with its length on the
/// grid. A file with no frames is a bounce that never closed; it is left
/// out rather than offered as a clip.
void SongEngine::Impl::doListClips() {
    std::vector<ClipInfo> out;
    std::string dir;
    uint32_t spBar = 0;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (opened_) {
            dir = song_.dir + "/" + AudioSong::clipsDir();
            spBar = song_.samplesPerBar(sampleRate_);
        }
    }
    if (!dir.empty() && p.storageReady()) {
        fsx::listDir(dir, [&](const std::string& name, bool) {
                if (name.size() < 5 || name.compare(name.size() - 4, 4, ".wav") != 0) return;
                /* Its length on the grid from the header alone: a bounce
                 * is a whole number of bars, anything else rounds up.
                 * Asked before every open: on the board an open that
                 * finds internal RAM short aborts rather than fails. */
                ClipReader r;
                if (p.storageReady() && r.open(dir + "/" + name) && r.frames && spBar) {
                    ClipInfo ci;
                    ci.file = name;
                    const uint32_t bars = (r.frames + spBar - 1) / spBar;
                    ci.bars = (uint16_t)std::max<uint32_t>(1, std::min<uint32_t>(bars, 999));
                    out.push_back(ci);
                }
                r.close();
                p.yieldIo();
        });
    }
    std::sort(out.begin(), out.end(), [](const ClipInfo& a, const ClipInfo& b) { return a.file < b.file; });
    std::lock_guard<std::mutex> l(listMutex_);
    clips_ = std::move(out);
}

/// Worker: open a project -- make its folders, read its song, remember
/// it. Read into a copy first, outside songMutex_, so the UI's timers are
/// not held out by the card; a song that cannot be read is not opened, and
/// the one open stays -- nothing then saves an empty song over it.
SongOpState SongEngine::Impl::doOpen(const std::string& name, bool remember) {
    if (!p.storageReady()) return SongOpState::Failed;
    /* The song open now has its last edits written first: a save waits
     * kSaveDelayMs after a commit, and the song it would have written is
     * replaced below -- a kit picked a moment before another song was
     * opened was lost, and so was any edit inside that window. Before
     * the read, too: reopening the same song reads what this writes. */
    if (saveReq_.exchange(false)) doSave();
    const std::string root = p.songsRoot();
    const std::string dir = root + "/" + name;
    /* An element's folder is two down: the library's, then its own. */
    const size_t slash = name.find('/');
    if (!fsx::makeDir(root) ||
        (slash != std::string::npos && !fsx::makeDir(root + "/" + name.substr(0, slash))) ||
        !fsx::makeDir(dir) || !fsx::makeDir(dir + "/" + AudioSong::clipsDir())) {
        return SongOpState::Failed;
    }
    AudioSong fresh;
    if (fresh.load(dir) == SongLoad::Unreadable) {
        logf(true, "%s/%s unreadable; left as it is", dir.c_str(), AudioSong::fileName());
        return SongOpState::Failed;
    }
    fresh.name = name;
    endPlayback();
    /* A freeze of a kit saved since, where no save was heard (another boot),
     * plays and bakes no more: the track plays live until frozen again. */
    const std::vector<int> stale = kitChangedFreezes(fresh);
    if (!stale.empty()) {
        logf(false, "%s: track %d's kit changed since it was frozen", name.c_str(), stale.front() + 1);
        unfreeze(fresh, stale);
    }
    {
        std::lock_guard<std::mutex> l(stretchMutex_);
        stretchReady_.clear();
        stretchSkip_.clear();
    }
    {
        /* The last song's notes are never asked for again: a song read anew
         * holds patterns of its own, the same song reopened too. Kept, they
         * would outlive it -- a few small blocks of internal RAM for every song
         * an element's enter or leave opens. */
        std::lock_guard<std::mutex> l(noteCacheMutex_);
        noteCache_.clear();
    }
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        /* An element open until now was left to come here: its blocks are baked first. */
        leftRef_ = opened_ && is_element_ref(song_.name) && song_.name != name ? song_.name : std::string();
        song_ = std::move(fresh);
        /* Another song: nothing of the last one's to undo. */
        undo_.clear();
        redo_.clear();
        undoCount_.store(0);
        redoCount_.store(0);
        pack(song_, last_);
        lastChangeUs_ = 0;
        /* The stem was the song around the element open until now. */
        dropContext(ContextState::None, {});
        rebuild();
    }
    /* Its element blocks are looked at again, whatever the last song's pass found:
     * a child may have changed on the card since this one was open. */
    relinkReq_.store(true);
    forgetElements();
    opened_ = true;
    if (remember) p.saveCurrent(name);
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        p.songOpened(song_);
    }
    return SongOpState::Ready;
}

/// Worker: the project last open, once the card is there. Tried once per
/// boot with a card present, whatever the answer: an unreadable song must
/// not be read again every second.
void SongEngine::Impl::openLast() {
    if (!p.storageReady()) return;
    lastTried_ = true;
    std::string name = p.loadCurrent();
    if (!valid_name(name)) name = "Song 1";
    openState_.store(doOpen(name));
}

/// Worker: the open project's folder under the name asked for, and that
/// name remembered as the one to open next time.
SongOpState SongEngine::Impl::doRename() {
    std::string name, from;
    bool isElement = false;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        name = editName_;
        from = song_.dir;
        isElement = is_element_ref(song_.name);
    }
    /* An element is named by its title; its folder is its identity in the
     * library, and a song's rename would move it out. */
    if (!opened_ || isElement || !p.storageReady()) return SongOpState::Failed;
    const std::string to = std::string(p.songsRoot()) + "/" + name;
    if (fsx::exists(to)) return SongOpState::Failed;              // taken
    endPlayback();
    if (!fsx::rename(from, to)) return SongOpState::Failed;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        song_.dir = to;
        song_.name = name;
        rebuild();
    }
    p.saveCurrent(name);
    return SongOpState::Ready;
}

/// Worker: the open project's folder gone, and another open in its place.
/// Only ever a folder straight under the songs root, by a valid name: a
/// song whose dir says anything else is refused rather than removed.
SongOpState SongEngine::Impl::doDelete() {
    std::string name, dir;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        name = song_.name;
        dir = song_.dir;
    }
    if (!opened_ || !p.storageReady() || !valid_name(name) ||
        dir != std::string(p.songsRoot()) + "/" + name) {
        return SongOpState::Failed;
    }
    endPlayback();
    /* Nothing may save the song being removed back onto the card: until
     * another is open there is none. */
    opened_ = false;
    saveReq_.store(false);
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        song_ = AudioSong();
        rebuild();
    }
    const bool removed = fsx::removeTree(dir, [this] { p.yieldIo(); });
    if (!removed) logf(true, "%s: not all of it could be removed", dir.c_str());
    doListProjects();
    listGen_.fetch_add(1);
    std::string next = "Song 1";
    {
        std::lock_guard<std::mutex> l(listMutex_);
        if (!projects_.empty()) next = projects_.front();
    }
    openState_.store(doOpen(next));
    return removed ? SongOpState::Ready : SongOpState::Failed;
}

/// Worker: write the song. A copy is taken under the lock so the card
/// write does not hold the UI out.
void SongEngine::Impl::doSave() {
    AudioSong copy;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (!opened_) return;
        copy = song_;
    }
    const int64_t t0 = p.nowUs();
    if (p.storageReady() && !copy.save()) logf(true, "song.json not written");
    dbgMax(dbgSaveMaxUs_, p.nowUs() - t0);
}

bool SongEngine::Impl::peaksPending() {
    std::lock_guard<std::mutex> l(peaksMutex_);
    return !peaksWanted_.empty();
}

/// Worker: the first waveform asked for -- from .peaks/ when that was
/// made from the same file, else read from the WAV and kept there. A
/// request coming in stops the reading; it is started again after.
void SongEngine::Impl::doPeaks() {
    std::string dir, file;
    {
        std::lock_guard<std::mutex> l(peaksMutex_);
        if (peaksWanted_.empty()) return;
        dir = peaksDir_;
        file = peaksWanted_.front();
    }
    auto values = std::make_shared<std::vector<uint8_t>>();
    const std::string wav = dir + "/" + AudioSong::clipsDir() + "/" + file;
    const std::string cache = dir + "/" + kPeaksDir + "/" + file + ".pk";
    uint64_t bytes = 0;
    int64_t modified = 0;
    /* No file: the clip is still cached, with nothing in it, so it is not
     * read for again on every frame -- but it is remembered as missing,
     * which is a different thing from a clip that is simply quiet. A song
     * folder that has been through another computer comes back without some
     * of its audio, and a screen has to be able to say so. */
    const bool here = fsx::fileInfo(wav, bytes, modified);
    if (!here) logf(false, "clip %s has no file in clips/", file.c_str());
    if (here && !loadPeaks(cache, bytes, *values)) {
        if (!readPeaks(wav, *values)) return;
        if (!values->empty()) savePeaks(dir, cache, bytes, *values);
    }
    {
        std::lock_guard<std::mutex> l(peaksMutex_);
        if (dir != peaksDir_) return;           // another project opened meanwhile
        peaks_[file] = std::move(values);
        if (here) peaksMissing_.erase(file);
        else      peaksMissing_.insert(file);
        peaksWanted_.erase(std::remove(peaksWanted_.begin(), peaksWanted_.end(), file), peaksWanted_.end());
    }
    peaksGen_.fetch_add(1);
}

/// The loudest sample of every kPeakHop frames of @p wav. False when a
/// request came in first; a file the engine cannot play gives none.
bool SongEngine::Impl::readPeaks(const std::string& wav, std::vector<uint8_t>& out) {
    ClipReader r;
    if (!r.open(wav)) return true;
    std::vector<int16_t> buf(kPeakChunkFrames * 2);
    out.reserve((r.frames + kPeakHop - 1) / kPeakHop);
    int peak = 0;
    uint32_t inHop = 0;
    for (uint32_t f = 0; f < r.frames; f += kPeakChunkFrames) {
        if (requestWaiting()) return false;
        const uint32_t n = std::min(kPeakChunkFrames, r.frames - f);
        r.read(f, buf.data(), n);
        for (uint32_t i = 0; i < n * 2; i += 2) {
            peak = std::max({peak, std::abs((int)buf[i]), std::abs((int)buf[i + 1])});
            if (++inHop == kPeakHop) {
                out.push_back((uint8_t)std::min(255, peak >> 7));
                peak = 0;
                inHop = 0;
            }
        }
        p.yieldIo();
    }
    if (inHop) out.push_back((uint8_t)std::min(255, peak >> 7));
    return true;
}

bool SongEngine::Impl::loadPeaks(const std::string& path, uint64_t wavBytes, std::vector<uint8_t>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t h[kPeaksHeader];
    bool ok = std::fread(h, 1, sizeof(h), f) == sizeof(h) && !std::memcmp(h, "CPPK", 4) &&
              h[4] == kPeaksVersion && (uint32_t)(h[6] | h[7] << 8) == kPeakHop &&
              (h[8] | h[9] << 8 | h[10] << 16 | (uint32_t)h[11] << 24) == (uint32_t)wavBytes;
    if (ok) {
        const size_t want = h[12] | h[13] << 8 | h[14] << 16 | (size_t)h[15] << 24;
        /* Against the cache file's own length, not only against the header:
         * a .pk that was truncated or came back damaged from another
         * computer would otherwise ask for a resize of whatever four bytes
         * happened to be there. */
        std::fseek(f, 0, SEEK_END);
        const long len = std::ftell(f);
        std::fseek(f, (long)kPeaksHeader, SEEK_SET);
        const size_t have = len > (long)kPeaksHeader ? (size_t)(len - (long)kPeaksHeader) : 0;
        ok = want && want <= have;
        if (ok) {
            out.resize(want);
            ok = std::fread(out.data(), 1, out.size(), f) == out.size();
        }
    }
    std::fclose(f);
    if (!ok) out.clear();
    return ok;
}

void SongEngine::Impl::savePeaks(const std::string& dir, const std::string& path, uint64_t wavBytes,
                                 const std::vector<uint8_t>& v) {
    if (!fsx::makeDir(dir + "/" + kPeaksDir)) return;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    const uint32_t b = (uint32_t)wavBytes, n = (uint32_t)v.size();
    const uint8_t h[kPeaksHeader] = {'C', 'P', 'P', 'K', kPeaksVersion, 0,
                                     (uint8_t)kPeakHop, (uint8_t)(kPeakHop >> 8),
                                     (uint8_t)b, (uint8_t)(b >> 8), (uint8_t)(b >> 16), (uint8_t)(b >> 24),
                                     (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
    const bool ok = std::fwrite(h, 1, sizeof(h), f) == sizeof(h) &&
                    std::fwrite(v.data(), 1, v.size(), f) == v.size();
    if (std::fclose(f) != 0 || !ok) std::remove(path.c_str());   // half a file would read as whole
}

} // namespace crosspad

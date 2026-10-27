// SPDX-License-Identifier: GPL-3.0-or-later
//
// SongEngine, the elements: the library's list, and mixing the open song down to its bake.
// An element is a project folder of its own under <songsRoot>/.elements/; the bake is the one WAV
// a song plays it from, in <songsRoot>/.bake/, named by what it was mixed from (ElementHash.hpp).
// The state is SongEngine::Impl (SongEngineImpl.hpp); the worker loop is in SongEngine.cpp.

#include "SongEngineImpl.hpp"

#include "crosspad/sequencer/SequenceStore.hpp"

#include "crosspad/song/ElementHash.hpp"
#include "crosspad/song/ElementResolver.hpp"
#include "crosspad/song/SongFiles.hpp"

namespace crosspad {

namespace {

/// The most a kit.json or an instrument.json is read whole for the hash.
constexpr size_t kMaxHashedFileBytes = 1u << 20;
constexpr size_t kHashReadChunk      = 512;
/// A copy's progress moves one step for each this many pieces copied (4 kB each:
/// a step a megabyte), so a long copy is seen to be going on.
constexpr uint32_t kCopyPiecesPerStep = 256;
/// An element's files as they were before an edit, in its own folder; the
/// song's is written last, so one that is there is a whole snapshot.
constexpr const char* kSongSnapshot = AudioSong::snapshotFileName();
constexpr char kScenesSnapshot[] = "sequences.orig.json";
constexpr char kScenesFile[]     = "sequences.json";
/// A file restored from a snapshot, written whole beside the one it replaces.
constexpr char kRestoredSuffix[] = ".restored";

} // namespace

/// Worker: the element folders under the songs root, each with what song.json
/// says of it. A folder that is not an element's name, or whose song cannot be
/// read, is not listed; one with no song.json yet (opened, not yet edited) is,
/// empty.
void SongEngine::Impl::doListElements() {
    std::vector<ElementInfo> out;
    std::set<std::string> snapshots;
    std::string open;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        open = song_.name;
    }
    if (p.storageReady()) {
        const std::string root = p.songsRoot();
        const std::string library = root + "/" + AudioSong::elementsDir();
        std::vector<std::string> names;
        fsx::listDir(library, [&](const std::string& name, bool isDir) {
            if (isDir && is_element_ref(std::string(AudioSong::elementsDir()) + "/" + name)) names.push_back(name);
        });
        for (const std::string& name : names) {
            AudioSong s;
            if (s.load(library + "/" + name) == SongLoad::Unreadable) continue;
            ElementInfo info;
            info.ref = std::string(AudioSong::elementsDir()) + "/" + name;
            info.title = s.title;
            info.bars = (uint16_t)std::min<uint32_t>(s.playEnd(), UINT16_MAX);
            for (const SongTrack& tr : s.tracks) {
                for (const SongClip& c : tr.clips) {
                    if (c.isElement() && std::find(info.uses.begin(), info.uses.end(), c.element) == info.uses.end())
                        info.uses.push_back(c.element);
                }
            }
            std::sort(info.uses.begin(), info.uses.end());
            const std::string dir = library + "/" + name;
            /* The element being edited is as the card has it until its first save:
             * its snapshot is no stale one. */
            if (info.ref != open && snapshotUnchanged(dir)) {
                /* An edit undone, or a drop that never ran: nothing to decide. */
                fsx::fs().remove((dir + "/" + kSongSnapshot).c_str());
                fsx::fs().remove((dir + "/" + kScenesSnapshot).c_str());
            } else if (fsx::exists(dir + "/" + kSongSnapshot)) {
                snapshots.insert(info.ref);
            }
            out.push_back(std::move(info));
            p.yieldIo();
        }
    }
    std::sort(out.begin(), out.end(), [](const ElementInfo& a, const ElementInfo& b) { return a.ref < b.ref; });
    std::lock_guard<std::mutex> l(listMutex_);
    elements_ = std::move(out);
    snapshots_ = std::move(snapshots);
}

bool SongEngine::Impl::snapshotUnchanged(const std::string& dir) {
    if (!fsx::exists(dir + "/" + kSongSnapshot)) return false;
    ElementHashReader rd = cardHashReader();
    /* A file absent on both sides is the same; one that cannot be read whole is not. */
    const auto same = [&](const std::string& kept, const std::string& live) {
        const bool k = fsx::exists(kept), l = fsx::exists(live);
        if (!k || !l) return !k && !l;
        std::string a, b;
        return rd.read(kept, a) && rd.read(live, b) && a == b;
    };
    return same(dir + "/" + kSongSnapshot, dir + "/" + AudioSong::fileName()) &&
           same(dir + "/" + kScenesSnapshot, dir + "/" + kScenesFile);
}

/// Worker: who plays each element -- every project's song.json and every
/// element's, one yield after each. What is open is counted as it is in memory:
/// its last edits may not be on the card yet.
void SongEngine::Impl::doListUses() {
    std::map<std::string, std::vector<ElementUse>> out;
    if (p.storageReady()) {
        const std::string root = p.songsRoot();
        const std::string library = root + "/" + AudioSong::elementsDir();
        std::vector<std::string> projects, elements;
        fsx::listDir(root, [&](const std::string& name, bool isDir) {
            if (isDir && name[0] != '.' && valid_name(name)) projects.push_back(name);
        });
        fsx::listDir(library, [&](const std::string& name, bool isDir) {
            if (isDir && is_element_ref(std::string(AudioSong::elementsDir()) + "/" + name))
                elements.push_back(std::string(AudioSong::elementsDir()) + "/" + name);
        });
        std::sort(projects.begin(), projects.end());
        std::sort(elements.begin(), elements.end());
        AudioSong open;
        bool haveOpen = false;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            if (opened_) {
                open = song_;
                haveOpen = true;
            }
        }
        auto scan = [&](const std::string& user, bool isSong) {
            AudioSong s;
            const AudioSong* src = &s;
            if (haveOpen && open.name == user) src = &open;
            else if (s.load(root + "/" + user) == SongLoad::Unreadable) return;
            std::set<std::string> seen;
            for (const SongTrack& tr : src->tracks) {
                for (const SongClip& c : tr.clips) {
                    if (c.isElement() && is_element_ref(c.element) && seen.insert(c.element).second)
                        out[c.element].push_back({user, isSong});
                }
            }
            p.yieldIo();
        };
        for (const std::string& name : projects) scan(name, true);
        for (const std::string& ref : elements) scan(ref, false);
    }
    std::lock_guard<std::mutex> l(listMutex_);
    uses_ = std::move(out);
}

bool SongEngine::Impl::requestUses() {
    if (!ensureWorker()) return false;
    usesAsked_.fetch_add(1);
    return request(kListUses);
}

ElementUses SongEngine::Impl::usesIndex() {
    std::lock_guard<std::mutex> l(listMutex_);
    return uses_;
}

std::vector<ElementUse> SongEngine::Impl::usesOf(const std::string& ref) {
    std::lock_guard<std::mutex> l(listMutex_);
    const auto it = uses_.find(ref);
    return it == uses_.end() ? std::vector<ElementUse>{} : it->second;
}

bool SongEngine::Impl::snapshotPending(const std::string& ref) {
    std::lock_guard<std::mutex> l(listMutex_);
    return snapshots_.count(ref) != 0;
}

bool SongEngine::Impl::requestSnapshotEdit(const std::string& ref, uint8_t edit) {
    if (!is_element_ref(ref) || !ensureWorker() || !claimEdit()) return false;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        editName_ = ref;
    }
    return requestEdit(edit);
}

ElementHashReader SongEngine::Impl::cardHashReader() {
    ElementHashReader rd;
    rd.info = [](const std::string& path, uint64_t& bytes, int64_t& modified) {
        FileStat st;
        if (fsx::fs().stat(path.c_str(), &st) != FsStat::Found || st.isDir) return false;
        bytes = st.size;
        modified = st.modified;
        return true;
    };
    rd.read = [](const std::string& path, std::string& bytes) {
        const int fd = fsx::openRead(path);
        if (fd < 0) return false;
        char buf[kHashReadChunk];
        bytes.clear();
        for (long n; (n = fsx::readSome(fd, buf, sizeof buf)) > 0 && bytes.size() <= kMaxHashedFileBytes;)
            bytes.append(buf, (size_t)n);
        fsx::close(fd);
        return bytes.size() <= kMaxHashedFileBytes;
    };
    rd.list = [](const std::string& dir, const std::function<void(const std::string&)>& each) {
        fsx::listDir(dir, [&](const std::string& name, bool isDir) { if (!isDir) each(dir + "/" + name); });
    };
    return rd;
}

bool SongEngine::Impl::requestBakeElement() {
    if (!ensureWorker() || !claimEdit()) return false;
    editProgress_.store(0);
    return requestEdit(kEditBake);
}

ElementBake SongEngine::Impl::bakeResult() {
    std::lock_guard<std::mutex> l(reportMutex_);
    return bake_;
}

/// Worker: the open song mixed down into the bake cache -- see
/// ISongEngine::requestBakeElement. Read from a copy and touching nothing of the
/// song: no clip is placed, nothing is saved. The mix is merge's -- each track
/// at its volume and pan, as the song plays it -- over every track that is heard, from bar 0 to the song's end; a seq track is heard as its
/// freeze, which plays in its place whether it is armed or not (a bake is not a
/// take), and one that is not frozen cannot be mixed from the card.
SongOpState SongEngine::Impl::doBakeElement() {
    {
        std::lock_guard<std::mutex> l(reportMutex_);
        report_.clear();
        bake_ = ElementBake{};
    }
    auto fail = [&](const std::string& why) {
        logf(true, "bake: %s", why.c_str());
        std::lock_guard<std::mutex> l(reportMutex_);
        report_.push_back(why);
        return SongOpState::Failed;
    };
    if (!opened_ || !p.storageReady() || !io_ || !acc_) return fail("No song open, or no card.");
    AudioSong copy;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        copy = song_;
    }
    copy.armed = -1;
    const uint32_t spBar = copy.samplesPerBar(sampleRate_);
    /* A kit saved under its name since the song opened, before anything heard
     * of it: those tracks are no longer frozen, and the bake waits for them. */
    const std::vector<int> stale = kitChangedFreezes(copy);
    if (!stale.empty()) {
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            if (song_.name == copy.name) {
                unfreeze(song_, stale);
                rebuild();
                saveAtUs_.store(p.nowUs() + (int64_t)kSaveDelayMs * 1000);
                saveReq_.store(true);
            }
        }
        for (int t : stale)
            if (!copy.silenced(t))
                return fail("Track " + std::to_string(t + 1) + "'s kit changed since it was frozen: freeze it again.");
    }
    for (int t = 0; t < kTracks; ++t) {
        const SongTrack& tr = copy.tracks[t];
        if (copy.silenced(t) || copy.seqPlaysNothing(t)) continue;
        if (tr.kind == TrackKind::Seq) {
            if (tr.freeze.empty() || tr.freezeKey != seqKey(copy, t))
                return fail("Track " + std::to_string(t + 1) + " is not frozen: freeze it first.");
            continue;
        }
        if (tr.stretch == StretchMode::Off) continue;
        std::lock_guard<std::mutex> l(stretchMutex_);
        for (const SongClip& c : tr.clips) {
            if (c.file.empty() || c.isElement()) continue;
            const std::string dst = copy.clipPath(stretchName(c, c.bars * spBar, tr.stretch));
            if (!stretchReady_.count(dst) && !stretchSkip_.count(dst))
                return fail("Track " + std::to_string(t + 1) + " is not fitted to the tempo yet: wait for it.");
        }
    }
    const uint32_t bars = copy.playEnd();
    if (!bars) return fail("The song is empty.");
    const uint32_t total = bars * spBar;

    /* Keyed as the resolver keys it from the song above, at the tempo it is
     * played at there (this one's, after ElementNav's fit; the default for a
     * song never given one), so the song above finds this bake by its key. */
    const float bpm = copy.bpm > 0.0f ? copy.bpm : AudioSong::kDefaultBpm;
    ElementHashReader rd = cardHashReader();
    ElementResolver children(p.songsRoot(), sampleRate_, bpm, rd);
    /* A block plays its element's bake as it is now when that is on the card,
     * and is named by it in the key; one whose element has no such bake yet is
     * mixed and named by the bake it holds, varispeed when it was made at
     * another tempo -- a bake of an older child is never named as one of the
     * newer. */
    for (SongTrack& tr : copy.tracks) {
        for (SongClip& c : tr.clips) {
            if (!c.isElement()) continue;
            const ResolvedElement& e = children.resolve(c.element);
            if (!e.exists) continue;
            c.file = e.file;
            c.frames = e.frames;
            c.bakeBpm = e.bpm;
        }
    }
    rd.childKey = [&children](const std::string& ref, uint64_t& key) {
        const ResolvedElement& e = children.resolve(ref);
        key = e.key;
        return e.exists;
    };
    const uint64_t key = element_bake_key(element_content_hash(copy, copy.name, rd), bpm, sampleRate_);
    ElementBake result;
    result.file = element_bake_file(key);
    result.bars = (uint16_t)std::min<uint32_t>(bars, UINT16_MAX);
    const std::string path = bakePath(result.file);

    if (fsx::exists(path)) {
        ClipReader found;
        if (found.open(path) && found.frames) {
            result.frames = found.frames;
            result.cached = true;
            ownBake(result.file);
            std::lock_guard<std::mutex> l(reportMutex_);
            bake_ = result;
            return SongOpState::Ready;
        }
        found.close();
        std::remove(path.c_str());   // cut short by something other than this: made again
    }

    const std::string cache = std::string(p.songsRoot()) + "/" + AudioSong::bakeDir();
    if (!fsx::makeDir(cache)) return fail("The card would not take the bake folder.");
    const Arrangement arr = flatten(copy);
    endPlayback();
    const std::string part = path + ".part";
    WavWriter w;
    if (!w.open(part.c_str(), sampleRate_, 2)) return fail("The bake could not be started on the card.");
    w.reserve(total);
    ClipReader rdr[kTracks];
    bool ok = true;
    for (uint32_t f = 0; f < total && ok;) {
        const uint32_t n = std::min(sz.chunkFrames, total - f);
        std::memset(acc_, 0, n * 8);
        for (int t = 0; t < kTracks; ++t) {
            if (copy.silenced(t)) continue;
            float gs[2];
            pan_side_gains(copy.tracks[t].volume, copy.tracks[t].pan, gs);
            for (const auto& sg : arr.tracks[t]) {
                if (sg.end <= f) continue;
                if (sg.start >= f + n) break;
                const uint32_t x = std::max(sg.start, f), y = std::min(sg.end, f + n);
                if (rdr[t].path != sg.path && !(p.storageReady() && rdr[t].open(sg.path))) continue;
                readAt(rdr[t], x - sg.start + sg.offset, sg.step, io_, y - x);
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
        editProgress_.store((uint8_t)((uint64_t)f * kBakeMixedPercent / total));
        p.yieldIo();
    }
    for (auto& r : rdr) r.close();
    if (!w.close() || !ok || !fsx::rename(part, path)) {
        std::remove(part.c_str());
        return fail("The bake was not written: the card is full or would not take it.");
    }
    editProgress_.store(100);
    result.frames = total;
    logf(false, "baked %u bars into %s", (unsigned)bars, result.file.c_str());
    ownBake(result.file);
    std::lock_guard<std::mutex> l(reportMutex_);
    bake_ = result;
    return SongOpState::Ready;
}

bool SongEngine::Impl::requestElementFromRegion(uint32_t fromBar, uint32_t toBar) {
    if (toBar <= fromBar || !ensureWorker() || !claimEdit()) return false;
    editFrom_.store(fromBar);
    editTo_.store(toBar);
    editProgress_.store(0);
    return requestEdit(kEditElementRegion);
}

bool SongEngine::Impl::requestElementFromSong(const std::string& name, const std::vector<std::string>& openElements) {
    if (!valid_name(name) || !ensureWorker() || !claimEdit()) return false;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        editName_ = name;
        editRefs_ = openElements;
    }
    editProgress_.store(0);
    return requestEdit(kEditElementSong);
}

bool SongEngine::Impl::requestElementDiscard(const std::string& ref) {
    if (!is_element_ref(ref) || !ensureWorker() || !claimEdit()) return false;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        editName_ = ref;
    }
    return requestEdit(kEditElementDiscard);
}

std::string SongEngine::Impl::elementMade() {
    std::lock_guard<std::mutex> l(reportMutex_);
    return elementMade_;
}

std::string SongEngine::Impl::makeElementFolder(std::string& dir, bool withClips) {
    const std::string root = p.songsRoot();
    const std::string library = root + "/" + AudioSong::elementsDir();
    dir.clear();
    if (!fsx::makeDir(root) || !fsx::makeDir(library)) return {};
    uint32_t seed = (uint32_t)p.nowUs();
    std::string ref;
    do {
        ref = std::string(AudioSong::elementsDir()) + "/" + make_element_id(seed++);
        dir = root + "/" + ref;
    } while (fsx::exists(dir));
    if (!withClips) return ref;
    if (!fsx::makeDir(dir) || !fsx::makeDir(dir + "/" + AudioSong::clipsDir())) return {};
    return ref;
}

SongOpState SongEngine::Impl::elementFailed(const std::string& why, const std::string& dir) {
    logf(true, "element: %s", why.c_str());
    if (!dir.empty() && fsx::exists(dir)) fsx::removeTree(dir, [this] { p.yieldIo(); });
    std::lock_guard<std::mutex> l(reportMutex_);
    report_.push_back(why);
    elementMade_.clear();
    return SongOpState::Failed;
}

/// Worker: a new element of the open song's [from, to) bars -- see
/// ISongEngine::requestElementFromRegion. The song is read from a copy and
/// never changed; what is written is a folder of its own under the library,
/// which is taken off the card again when any piece of it fails.
SongOpState SongEngine::Impl::doElementFromRegion() {
    {
        std::lock_guard<std::mutex> l(reportMutex_);
        report_.clear();
        elementMade_.clear();
    }
    if (!opened_ || !p.storageReady()) return elementFailed("No song open, or no card.", {});
    const uint32_t from = editFrom_.load(), to = editTo_.load();
    AudioSong src;
    std::vector<RangeClip> parts;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        src = song_;
        /* A block that plays its track's scene is given the scene's notes, as
         * the seq view does before it edits: the element has no scenes. */
        const uint32_t bars = std::max(endBars(src), to);
        for (int t = 0; t < kTracks; ++t) src.adoptSceneNotes(t, scenePattern(src.tracks[t].scene), bars);
        parts = src.copyRange(from, to, src.samplesPerBar(sampleRate_));
    }
    if (parts.empty()) return elementFailed("Nothing plays in those bars.", {});
    /* Names that reach outside the folders are refused before anything is written. */
    for (const RangeClip& k : parts) {
        if (!k.clip.isElement() && src.tracks[k.track].kind == TrackKind::Audio && !is_song_clip_name(k.clip.file))
            return elementFailed("A recording's name is not a plain file name: " + k.clip.file, {});
    }
    std::string kept;   // the first component of an instrument the song keeps in its own folder
    if (!src.instrument.empty() && src.instrument[0] != '/') {
        kept = src.instrument.substr(0, src.instrument.find('/'));
        if (!valid_name(kept)) return elementFailed("The song's instrument is not inside its own folder.", {});
    }

    std::string dir;
    const std::string ref = makeElementFolder(dir, true);
    if (ref.empty()) return elementFailed("The card would not take the element's folder.", dir);
    AudioSong el;
    el.dir = dir;
    el.name = ref;
    el.bpm = src.bpm;
    el.beatsPerBar = src.beatsPerBar;
    el.loop = src.loop;
    el.length = to - from;
    el.kit = src.kit;
    el.instrument = src.instrument;
    for (const RangeClip& k : parts) {
        SongTrack& d = el.tracks[k.track];
        if (!d.inUse) {
            const SongTrack& s = src.tracks[k.track];
            d.inUse = true;
            d.name = s.name;
            d.volume = s.volume;
            d.pan = s.pan;
            d.muted = s.muted;
            d.solo = s.solo;
            d.kind = s.kind;
            d.source = s.source;
            d.scene = s.scene;
            d.seqBlocks = s.seqBlocks;
            d.stretch = s.stretch;
            d.input = s.input;
            d.kit = s.kit;
        }
        SongClip c = k.clip;
        c.bar = k.rel;
        d.clips.push_back(c);
    }
    for (int t = 0; t < kTracks; ++t) el.sortTrack(t);
    /* ARM where it was, when that track came along: CLIP EDIT into it and a DUB
     * goes onto the part being played, not the first seq track (armed -1). */
    if (src.armed >= 0 && src.armed < kTracks && el.tracks[src.armed].inUse) el.armed = src.armed;

    /* The recordings the blocks play, once each; an element block plays an
     * element's bake, which is no file of the song's. */
    std::vector<std::string> copied;
    const auto yield = [this] { p.yieldIo(); };
    for (const RangeClip& k : parts) {
        const std::string& file = k.clip.file;
        if (file.empty() || k.clip.isElement() || std::find(copied.begin(), copied.end(), file) != copied.end()) continue;
        /* A bounced track's audio is in the freeze folder, and goes to the element's. */
        if (file.find('/') != std::string::npos) fsx::makeDir(el.clipPath(kFreezeDir));
        if (!song_copy_file(src.clipPath(file), el.clipPath(file), yield))
            return elementFailed("The recording " + file + " could not be copied.", dir);
        copied.push_back(file);
    }
    /* An instrument the song keeps is a folder of its own beside the clips. */
    if (!kept.empty()) {
        const bool isFolder = kept != src.instrument;
        const bool ok = isFolder ? song_copy_tree(src.dir + "/" + kept, dir + "/" + kept,
                                                  [](const std::string&) { return true; }, yield)
                                 : song_copy_file(src.dir + "/" + kept, dir + "/" + kept, yield);
        if (!ok) return elementFailed("The song's instrument could not be copied.", dir);
    }
    if (!el.save()) return elementFailed("The element's song could not be written.", dir);
    /* The song's scenes with it: its kit tracks name them (SongTrack::scene),
     * and an element with no scenes of its own was given the card's old shared
     * file on open -- other scenes, and every freeze of a track on a scene it
     * had not got failed ("no such scene"), so the element never baked. */
    if (el.seqTracks() > 0 && !SequenceStore::save(seq_, dir + "/" + SequenceStore::fileName()))
        return elementFailed("The song's scenes could not be written into the element.", dir);
    editProgress_.store(100);
    logf(false, "element %s made of bars %u-%u", ref.c_str(), (unsigned)from + 1, (unsigned)to);
    std::lock_guard<std::mutex> l(reportMutex_);
    elementMade_ = ref;
    return SongOpState::Ready;
}

/// Worker: the project asked for copied into the library as an element -- see
/// ISongEngine::requestElementFromSong. Its folder as it is on the card, less
/// what is made again from it (freezes, fitted copies), and its song.json
/// rewritten as the element's.
SongOpState SongEngine::Impl::doElementFromSong() {
    std::string name;
    std::vector<std::string> open;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        name = editName_;
        open = editRefs_;
    }
    {
        std::lock_guard<std::mutex> l(reportMutex_);
        report_.clear();
        elementMade_.clear();
    }
    if (!p.storageReady()) return elementFailed("No card.", {});
    if (saveReq_.exchange(false)) doSave();   // the song may be the open one: its last edits go first
    const std::string from = std::string(p.songsRoot()) + "/" + name;
    AudioSong s;
    if (s.load(from) != SongLoad::Loaded) return elementFailed("There is no song called " + name + ".", {});
    /* The elements the song plays, and the ones those play: one on the breadcrumb
     * would make the element hold itself. Read from the library's song.json. */
    std::vector<std::string> reached;
    const auto reach = [&](const AudioSong& song) {
        for (const SongTrack& t : song.tracks)
            for (const SongClip& c : t.clips)
                if (c.isElement() && std::find(reached.begin(), reached.end(), c.element) == reached.end())
                    reached.push_back(c.element);
    };
    reach(s);
    for (size_t i = 0; i < reached.size(); ++i) {
        if (std::find(open.begin(), open.end(), reached[i]) != open.end())
            return elementFailed("An element cannot hold itself.", {});
        AudioSong child;
        if (is_element_ref(reached[i]) && child.load(std::string(p.songsRoot()) + "/" + reached[i]) == SongLoad::Loaded)
            reach(child);
        p.yieldIo();
    }
    std::string dir;
    const std::string ref = makeElementFolder(dir, false);
    if (ref.empty()) return elementFailed("The card would not take the element's folder.", dir);
    const std::string clips = std::string(AudioSong::clipsDir()) + "/";
    const auto keep = [&](const std::string& rel) {
        return rel.rfind(AudioSong::fileName(), 0) != 0 && rel != clips + kFreezeDir && rel != clips + kStretchDir;
    };
    if (!song_copy_tree(from, dir, keep, [this] { p.yieldIo(); }))
        return elementFailed("The song could not be copied.", dir);
    s.dir = dir;
    s.name = ref;
    s.title = name;
    s.armed = -1;
    for (SongTrack& t : s.tracks) {
        t.freeze.clear();
        t.freezeKey = 0;
    }
    if (!s.save()) return elementFailed("The element's song could not be written.", dir);
    editProgress_.store(100);
    logf(false, "element %s copied from %s", ref.c_str(), name.c_str());
    std::lock_guard<std::mutex> l(reportMutex_);
    elementMade_ = ref;
    return SongOpState::Ready;
}

/// Worker: an element made by one of the requests above, taken off the card
/// again -- see ISongEngine::requestElementDiscard.
SongOpState SongEngine::Impl::doElementDiscard() {
    std::string ref, open;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        ref = editName_;
        open = song_.name;
    }
    if (!p.storageReady() || ref == open) return SongOpState::Failed;
    const std::string dir = std::string(p.songsRoot()) + "/" + ref;
    if (fsx::exists(dir) && !fsx::removeTree(dir, [this] { p.yieldIo(); })) return SongOpState::Failed;
    return SongOpState::Ready;
}

/// Worker: the element's song.json and sequences.json kept beside it -- see
/// ISongEngine::requestSnapshot.
SongOpState SongEngine::Impl::doSnapshot() {
    std::string ref, open;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        ref = editName_;
        open = song_.name;
    }
    if (!p.storageReady() || ref == open) return SongOpState::Failed;
    const std::string dir = std::string(p.songsRoot()) + "/" + ref;
    const std::string song = dir + "/" + AudioSong::fileName(), scenes = dir + "/" + kScenesFile;
    const std::string songKept = dir + "/" + kSongSnapshot, scenesKept = dir + "/" + kScenesSnapshot;
    const auto yield = [this] { p.yieldIo(); };
    if (!fsx::exists(songKept) && fsx::exists(song)) {
        /* Scenes left by a power cut between the two copies are not this element's. */
        if (!fsx::exists(scenes) && fsx::exists(scenesKept) && !fsx::fs().remove(scenesKept.c_str()))
            return SongOpState::Failed;
        if ((fsx::exists(scenes) && !song_copy_file(scenes, scenesKept, yield)) || !song_copy_file(song, songKept, yield)) {
            fsx::fs().remove(scenesKept.c_str());
            fsx::fs().remove(songKept.c_str());
            return SongOpState::Failed;
        }
    }
    if (fsx::exists(songKept)) {
        std::lock_guard<std::mutex> l(listMutex_);
        snapshots_.insert(ref);
    }
    return SongOpState::Ready;
}

bool SongEngine::Impl::requestEditHash() {
    if (!ensureWorker() || !claimEdit()) return false;
    return requestEdit(kEditEditHash);
}

/// Worker: element_edit_hash() of the open song, from a copy, with the
/// project's scenes as the card holds them.
SongOpState SongEngine::Impl::doEditHash() {
    AudioSong copy;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        if (!opened_) return SongOpState::Failed;
        copy = song_;
    }
    std::string scenes;
    const std::string file = copy.dir + "/" + kScenesFile;
    if (p.storageReady() && fsx::exists(file) && !cardHashReader().read(file, scenes)) return SongOpState::Failed;
    editHash_.store(element_edit_hash(copy, scenes));
    return SongOpState::Ready;
}

/// Worker: the snapshot back over the element -- see ISongEngine::requestRestore.
SongOpState SongEngine::Impl::doRestore() {
    std::string ref, open;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        ref = editName_;
        open = song_.name;
    }
    /* Not under a song that streams: its card is the ring's. */
    if (!p.storageReady() || ref == open || play_.load() != Play::Idle) return SongOpState::Failed;
    if (!restoreSnapshot(std::string(p.songsRoot()) + "/" + ref)) return SongOpState::Failed;
    std::lock_guard<std::mutex> l(listMutex_);
    snapshots_.erase(ref);
    return SongOpState::Ready;
}

/// Both files are written beside the element first, so a card that fails there
/// leaves it as it was; only then does each step aside for its snapshot (to
/// .bak, which AudioSong and SequenceStore read when the file is gone), and the
/// snapshot goes last, so a failure after that can be restored from again.
bool SongEngine::Impl::restoreSnapshot(const std::string& dir) {
    const std::string song = dir + "/" + AudioSong::fileName(), scenes = dir + "/" + kScenesFile;
    const std::string songKept = dir + "/" + kSongSnapshot, scenesKept = dir + "/" + kScenesSnapshot;
    if (!fsx::exists(songKept)) return false;
    const auto yield = [this] { p.yieldIo(); };
    const bool hadScenes = fsx::exists(scenesKept);
    const std::string songNext = song + kRestoredSuffix, scenesNext = scenes + kRestoredSuffix;
    if ((hadScenes && !song_copy_file(scenesKept, scenesNext, yield)) || !song_copy_file(songKept, songNext, yield)) {
        fsx::fs().remove(scenesNext.c_str());
        fsx::fs().remove(songNext.c_str());
        return false;
    }
    const auto replace = [](const std::string& next, const std::string& file) {
        const std::string bak = file + ".bak";
        fsx::fs().remove(bak.c_str());
        const bool hadOld = fsx::exists(file) && fsx::rename(file, bak);
        if (!fsx::rename(next, file)) {
            if (hadOld) fsx::rename(bak, file);
            return false;
        }
        fsx::fs().remove(bak.c_str());
        return true;
    };
    if (hadScenes && !replace(scenesNext, scenes)) return false;
    /* The element had no scenes of its own when it was kept: one made since goes. */
    if (!hadScenes && fsx::exists(scenes) && !fsx::fs().remove(scenes.c_str())) return false;
    if (!replace(songNext, song)) return false;
    return dropSnapshotFiles(dir);
}

/// Worker: the snapshot taken off the card -- see ISongEngine::requestDropSnapshot.
/// The song's goes first: it is what makes a snapshot whole.
SongOpState SongEngine::Impl::doDropSnapshot() {
    std::string ref;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        ref = editName_;
    }
    if (!p.storageReady() || !dropSnapshotFiles(std::string(p.songsRoot()) + "/" + ref)) return SongOpState::Failed;
    std::lock_guard<std::mutex> l(listMutex_);
    snapshots_.erase(ref);
    return SongOpState::Ready;
}

/// The song's goes first: it is what makes a snapshot whole.
bool SongEngine::Impl::dropSnapshotFiles(const std::string& dir) {
    for (const char* name : {kSongSnapshot, kScenesSnapshot}) {
        const std::string path = dir + "/" + name;
        if (fsx::exists(path) && !fsx::fs().remove(path.c_str())) return false;
    }
    return true;
}

bool SongEngine::Impl::requestCopyElements(const std::vector<ElementFork>& forks, const std::vector<std::string>& rewrites,
                                           const std::map<std::string, std::string>& copied) {
    /* One original put back per request: a second failing after the first was
     * restored could not be undone. */
    if (std::count_if(forks.begin(), forks.end(), [](const ElementFork& f) { return f.restore; }) > 1) return false;
    for (const ElementFork& f : forks)
        if (!is_element_ref(f.ref)) return false;
    for (const std::string& r : rewrites)
        if (!is_element_ref(r)) return false;
    for (const auto& kv : copied)
        if (!is_element_ref(kv.first) || !is_element_ref(kv.second)) return false;
    if (!ensureWorker() || !claimEdit()) return false;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        copyForks_ = forks;
        copyRewrites_ = rewrites;
        copyRefs_ = copied;
    }
    {
        std::lock_guard<std::mutex> l(reportMutex_);
        copiesMade_.clear();
    }
    editProgress_.store(0);
    return requestEdit(kEditCopyElements);
}

std::map<std::string, std::string> SongEngine::Impl::elementsCopied() {
    std::lock_guard<std::mutex> l(reportMutex_);
    return copiesMade_;
}

/// Worker: see ISongEngine::requestCopyElements. Everything is read before
/// anything is written; the copies are new folders, so a failure takes them off
/// the card and writes back what the elements re-pointed held. An original goes
/// back to its snapshot last, once its copy is whole.
SongOpState SongEngine::Impl::doCopyElements() {
    std::vector<ElementFork> forks;
    std::vector<std::string> rewrites;
    std::map<std::string, std::string> refs;
    std::string open;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        forks = copyForks_;
        rewrites = copyRewrites_;
        refs = copyRefs_;
        open = song_.name;
    }
    {
        std::lock_guard<std::mutex> l(reportMutex_);
        report_.clear();
        copiesMade_.clear();
    }
    const auto yield = [this] { p.yieldIo(); };
    std::vector<std::string> made;    // the copies' folders
    std::vector<AudioSong> written;   // elements re-pointed, as they were
    auto fail = [&](const std::string& why) {
        logf(true, "copy: %s", why.c_str());
        for (const AudioSong& w : written) w.save();
        for (const std::string& dir : made) fsx::removeTree(dir, yield);
        std::lock_guard<std::mutex> l(reportMutex_);
        report_.push_back(why);
        return SongOpState::Failed;
    };
    if (!p.storageReady()) return fail("No card.");
    const std::string root = p.songsRoot();
    for (const ElementFork& f : forks) {
        if (f.ref == open) return fail("Element " + f.ref + " is open.");
        /* Not under a song that streams: its card is the ring's. */
        if (f.restore && play_.load() != Play::Idle) return fail("The song is playing.");
    }
    for (const std::string& r : rewrites)
        if (r == open) return fail("Element " + r + " is open.");

    std::vector<AudioSong> copies(forks.size()), held(rewrites.size());
    for (size_t i = 0; i < forks.size(); ++i)
        if (copies[i].load(root + "/" + forks[i].ref) != SongLoad::Loaded)
            return fail("Element " + forks[i].ref + " cannot be read.");
    for (size_t i = 0; i < rewrites.size(); ++i)
        if (held[i].load(root + "/" + rewrites[i]) != SongLoad::Loaded)
            return fail("Element " + rewrites[i] + " cannot be read.");

    /* The snapshot of an edit stays behind, and so do the copies fitted to a
     * tempo and the waveforms, made again from the copy as it is used. Its
     * freezes go with it: their keys name what the tracks play, not the folder,
     * so they hold, and the copy is baked from the card at once. */
    const std::string clips = std::string(AudioSong::clipsDir()) + "/";
    const auto keep = [&](const std::string& rel) {
        return rel.rfind(AudioSong::fileName(), 0) != 0 && rel.rfind(kSongSnapshot, 0) != 0 &&
               rel.rfind(kScenesSnapshot, 0) != 0 && rel != clips + kStretchDir && rel != kPeaksDir;
    };
    uint32_t pieces = 0;
    const auto copying = [&] {
        if (++pieces % kCopyPiecesPerStep == 0 && editProgress_.load() + 1 < kBakeMixedPercent)
            editProgress_.fetch_add(1);
        p.yieldIo();
    };
    std::map<std::string, std::string> copied;
    for (const ElementFork& f : forks) {
        std::string dir;
        const std::string ref = makeElementFolder(dir, false);
        if (ref.empty()) return fail("The card would not take a new element's folder.");
        made.push_back(dir);
        if (!song_copy_tree(root + "/" + f.ref, dir, keep, copying)) return fail("Element " + f.ref + " could not be copied.");
        copied[f.ref] = ref;
    }
    for (const auto& kv : copied) refs[kv.first] = kv.second;
    const auto repoint = [&](AudioSong& s) {
        bool any = false;
        for (SongTrack& t : s.tracks) {
            for (SongClip& c : t.clips) {
                const auto to = c.isElement() ? refs.find(c.element) : refs.end();
                if (to == refs.end()) continue;
                c.element = to->second;
                any = true;
            }
        }
        return any;
    };
    for (size_t i = 0; i < forks.size(); ++i) {
        AudioSong& s = copies[i];
        s.dir = made[i];
        s.name = copied[forks[i].ref];
        s.title = forks[i].title;
        s.armed = -1;
        repoint(s);
        if (!s.save()) return fail("The copy of " + forks[i].ref + " could not be written.");
    }
    for (size_t i = 0; i < rewrites.size(); ++i) {
        AudioSong s = held[i];
        if (!repoint(s)) continue;
        if (!s.save()) return fail("Element " + rewrites[i] + " could not be written.");
        written.push_back(held[i]);
    }
    for (const ElementFork& f : forks) {
        if (!f.restore) continue;
        if (!restoreSnapshot(root + "/" + f.ref)) return fail("Element " + f.ref + " could not be put back as it was.");
        std::lock_guard<std::mutex> l(listMutex_);
        snapshots_.erase(f.ref);
    }
    editProgress_.store(100);
    /* The song's blocks of elements whose content moved under them take new bakes. */
    relinkReq_.store(true);
    logf(false, "copied %u elements, re-pointed %u", (unsigned)forks.size(), (unsigned)written.size());
    std::lock_guard<std::mutex> l(reportMutex_);
    copiesMade_ = copied;
    return SongOpState::Ready;
}

bool SongEngine::Impl::requestDeleteElement(const std::string& ref) {
    if (!is_element_ref(ref) || play_.load() != Play::Idle || !ensureWorker() || !claimEdit()) return false;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        editName_ = ref;
    }
    return requestEdit(kEditDeleteElement);
}

/// Worker: see ISongEngine::requestDeleteElement. Who uses it is read afresh,
/// here, so a scan made before a song placed it cannot let it go.
SongOpState SongEngine::Impl::doDeleteElement() {
    std::string ref, open;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        ref = editName_;
        open = song_.name;
    }
    {
        std::lock_guard<std::mutex> l(reportMutex_);
        report_.clear();
    }
    auto fail = [&](const std::vector<std::string>& why) {
        std::lock_guard<std::mutex> l(reportMutex_);
        report_ = why;
        return SongOpState::Failed;
    };
    if (!p.storageReady()) return fail({"No card."});
    if (ref == open) return fail({"The element is open: leave it first."});
    /* The scan and the removal read the whole library: not under a song that streams. */
    if (play_.load() != Play::Idle) return fail({"The song is playing: stop it first."});
    const uint32_t asked = usesAsked_.load();
    doListUses();
    usesAnswered_.store(asked);
    listGen_.fetch_add(1);
    const std::vector<ElementUse> users = usesOf(ref);
    if (!users.empty()) {
        std::vector<std::string> why{"Used in " + std::to_string(users.size()) +
                                     (users.size() == 1 ? " place." : " places.")};
        {
            std::lock_guard<std::mutex> l(listMutex_);
            for (const ElementUse& u : users) {
                std::string name = u.user;
                for (const ElementInfo& e : elements_)
                    if (!u.song && e.ref == u.user && !e.title.empty()) name = e.title;
                why.push_back(name);
            }
        }
        return fail(why);
    }
    const std::string dir = std::string(p.songsRoot()) + "/" + ref;
    if (fsx::exists(dir) && !fsx::removeTree(dir, [this] { p.yieldIo(); }))
        return fail({"The card would not let go of the element."});
    {
        std::lock_guard<std::mutex> l(listMutex_);
        elements_.erase(std::remove_if(elements_.begin(), elements_.end(),
                                       [&](const ElementInfo& e) { return e.ref == ref; }),
                        elements_.end());
        snapshots_.erase(ref);
        for (auto& kv : uses_)
            kv.second.erase(std::remove_if(kv.second.begin(), kv.second.end(),
                                           [&](const ElementUse& u) { return !u.song && u.user == ref; }),
                            kv.second.end());
    }
    listGen_.fetch_add(1);
    logf(false, "element %s deleted", ref.c_str());
    return SongOpState::Ready;
}

} // namespace crosspad

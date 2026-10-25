// SPDX-License-Identifier: GPL-3.0-or-later
//
// SongEngine, the elements: the library's list, and mixing the open song down to its bake.
// An element is a project folder of its own under <songsRoot>/.elements/; the bake is the one WAV
// a song plays it from, in <songsRoot>/.bake/, named by what it was mixed from (ElementHash.hpp).
// The state is SongEngine::Impl (SongEngineImpl.hpp); the worker loop is in SongEngine.cpp.

#include "SongEngineImpl.hpp"

#include "crosspad/song/ElementHash.hpp"
#include "crosspad/song/ElementResolver.hpp"
#include "crosspad/song/SongFiles.hpp"

namespace crosspad {

namespace {

/// The most a kit.json or an instrument.json is read whole for the hash.
constexpr size_t kMaxHashedFileBytes = 1u << 20;
constexpr size_t kHashReadChunk      = 512;
/// An element's files as they were before an edit, in its own folder; the
/// song's is written last, so one that is there is a whole snapshot.
constexpr char kSongSnapshot[]   = "song.orig.json";
constexpr char kScenesSnapshot[] = "sequences.orig.json";
constexpr char kScenesFile[]     = "sequences.json";

} // namespace

/// Worker: the element folders under the songs root, each with what song.json
/// says of it. A folder that is not an element's name, or whose song cannot be
/// read, is not listed; one with no song.json yet (opened, not yet edited) is,
/// empty.
void SongEngine::Impl::doListElements() {
    std::vector<ElementInfo> out;
    std::set<std::string> snapshots;
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
            if (fsx::exists(library + "/" + name + "/" + kSongSnapshot)) snapshots.insert(info.ref);
            out.push_back(std::move(info));
            p.yieldIo();
        }
    }
    std::sort(out.begin(), out.end(), [](const ElementInfo& a, const ElementInfo& b) { return a.ref < b.ref; });
    std::lock_guard<std::mutex> l(listMutex_);
    elements_ = std::move(out);
    snapshots_ = std::move(snapshots);
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
    for (int t = 0; t < kTracks; ++t) {
        const SongTrack& tr = copy.tracks[t];
        if (copy.silenced(t)) continue;
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
     * mixed and named by the bake it holds -- a bake of an older child is never
     * named as one of the newer. */
    for (SongTrack& tr : copy.tracks) {
        for (SongClip& c : tr.clips) {
            if (!c.isElement()) continue;
            const ResolvedElement& e = children.resolve(c.element);
            if (!e.exists) continue;
            c.file = e.file;
            c.frames = e.frames;
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
    result.key = key;
    result.bars = (uint16_t)std::min<uint32_t>(bars, UINT16_MAX);
    const std::string path = bakePath(result.file);

    if (fsx::exists(path)) {
        ClipReader found;
        if (found.open(path) && found.frames) {
            result.frames = found.frames;
            result.cached = true;
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
                rdr[t].read(x - sg.start + sg.offset, io_, y - x);
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
        if (!k.clip.isElement() && src.tracks[k.track].kind == TrackKind::Audio && !is_clip_file_name(k.clip.file))
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

    /* The recordings the blocks play, once each; an element block plays an
     * element's bake, which is no file of the song's. */
    std::vector<std::string> copied;
    const auto yield = [this] { p.yieldIo(); };
    for (const RangeClip& k : parts) {
        const std::string& file = k.clip.file;
        if (file.empty() || k.clip.isElement() || std::find(copied.begin(), copied.end(), file) != copied.end()) continue;
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
/// ISongEngine::requestSnapshot. The element's key is read first, from the card
/// as it is, for the caller to tell later whether it was edited.
SongOpState SongEngine::Impl::doSnapshot() {
    std::string ref, open;
    float bpm = 0.0f;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        ref = editName_;
        open = song_.name;
        bpm = song_.bpm;
    }
    snapshotKey_.store(0);
    if (!p.storageReady() || ref == open) return SongOpState::Failed;
    const std::string root = p.songsRoot();
    ElementResolver resolver(root, sampleRate_, bpm > 0.0f ? bpm : AudioSong::kDefaultBpm, cardHashReader());
    snapshotKey_.store(resolver.resolve(ref).key);

    const std::string dir = root + "/" + ref;
    const std::string song = dir + "/" + AudioSong::fileName(), scenes = dir + "/" + kScenesFile;
    const std::string songKept = dir + "/" + kSongSnapshot, scenesKept = dir + "/" + kScenesSnapshot;
    const auto yield = [this] { p.yieldIo(); };
    if (!fsx::exists(songKept) && fsx::exists(song)) {
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

/// Worker: the snapshot back over the element -- see ISongEngine::requestRestore.
/// The scenes go first and the snapshot goes last, so a card that fails half
/// way still holds the snapshot to restore from again.
SongOpState SongEngine::Impl::doRestore() {
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
    if (!fsx::exists(songKept)) return SongOpState::Failed;
    const auto yield = [this] { p.yieldIo(); };
    const bool hadScenes = fsx::exists(scenesKept);
    if (hadScenes && !song_copy_file(scenesKept, scenes, yield)) return SongOpState::Failed;
    if (!song_copy_file(songKept, song, yield)) return SongOpState::Failed;
    /* The element had no scenes of its own when it was kept: one made since goes. */
    if (!hadScenes && fsx::exists(scenes)) fsx::fs().remove(scenes.c_str());
    return doDropSnapshot();
}

/// Worker: the snapshot taken off the card -- see ISongEngine::requestDropSnapshot.
/// The song's goes first: it is what makes a snapshot whole.
SongOpState SongEngine::Impl::doDropSnapshot() {
    std::string ref;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        ref = editName_;
    }
    if (!p.storageReady()) return SongOpState::Failed;
    const std::string dir = std::string(p.songsRoot()) + "/" + ref;
    for (const char* name : {kSongSnapshot, kScenesSnapshot}) {
        const std::string path = dir + "/" + name;
        if (fsx::exists(path) && !fsx::fs().remove(path.c_str())) return SongOpState::Failed;
    }
    std::lock_guard<std::mutex> l(listMutex_);
    snapshots_.erase(ref);
    return SongOpState::Ready;
}

} // namespace crosspad

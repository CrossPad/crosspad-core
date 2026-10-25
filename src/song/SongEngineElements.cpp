// SPDX-License-Identifier: GPL-3.0-or-later
//
// SongEngine, the elements: the library's list, and mixing the open song down to its bake.
// An element is a project folder of its own under <songsRoot>/.elements/; the bake is the one WAV
// a song plays it from, in <songsRoot>/.bake/, named by what it was mixed from (ElementHash.hpp).
// The state is SongEngine::Impl (SongEngineImpl.hpp); the worker loop is in SongEngine.cpp.

#include "SongEngineImpl.hpp"

#include "crosspad/song/ElementHash.hpp"
#include "crosspad/song/ElementResolver.hpp"

namespace crosspad {

namespace {

/// The most a kit.json or an instrument.json is read whole for the hash.
constexpr size_t kMaxHashedFileBytes = 1u << 20;
constexpr size_t kHashReadChunk      = 512;

} // namespace

/// Worker: the element folders under the songs root, each with what song.json
/// says of it. A folder that is not an element's name, or whose song cannot be
/// read, is not listed; one with no song.json yet (opened, not yet edited) is,
/// empty.
void SongEngine::Impl::doListElements() {
    std::vector<ElementInfo> out;
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
            out.push_back(std::move(info));
            p.yieldIo();
        }
    }
    std::sort(out.begin(), out.end(), [](const ElementInfo& a, const ElementInfo& b) { return a.ref < b.ref; });
    std::lock_guard<std::mutex> l(listMutex_);
    elements_ = std::move(out);
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
/// song: no clip is placed, nothing is saved. The mix is merge's, over every
/// track that is heard, from bar 0 to the song's end; a seq track is heard as its
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
            const float g = copy.tracks[t].volume;
            for (const auto& sg : arr.tracks[t]) {
                if (sg.end <= f) continue;
                if (sg.start >= f + n) break;
                const uint32_t x = std::max(sg.start, f), y = std::min(sg.end, f + n);
                if (rdr[t].path != sg.path && !(p.storageReady() && rdr[t].open(sg.path))) continue;
                rdr[t].read(x - sg.start + sg.offset, io_, y - x);
                int32_t* d = acc_ + (x - f) * 2;
                for (uint32_t k = 0; k < (y - x) * 2; ++k) d[k] += (int32_t)((float)io_[k] * g);
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

} // namespace crosspad

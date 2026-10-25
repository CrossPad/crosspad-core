// SPDX-License-Identifier: GPL-3.0-or-later
//
// The relink pass and the disk baker: element blocks of the open song linked to
// their element's bake as it is now, and bakes mixed from files on the card.

#include "song_engine_rig.hpp"

#include "crosspad/platform/IFileSystem.hpp"
#include "crosspad/song/SeqFreezeKey.hpp"

#include <fstream>
#include <iterator>
#include <set>

namespace {

const char* const kChild      = ".elements/E00000001";
const char* const kGrandchild = ".elements/E00000002";
/// A bake an element block played before its element changed.
const std::string kOldBake = "B00000000000000AA.wav";
constexpr int16_t kOld = 1000;
/// A bar at 120 BPM, and what a bake waits for at most in these tests.
constexpr uint32_t kBar = 2 * kRate;
constexpr int kBakeWaitMs = 10000;

fs::path bakeDir(Rig& r) { return fs::path(r.platform.root) / AudioSong::bakeDir(); }

std::set<std::string> bakes(Rig& r) {
    std::set<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(bakeDir(r), ec)) out.insert(e.path().filename().string());
    return out;
}

void writeOldBake(Rig& r) {
    fs::create_directories(bakeDir(r));
    writeClip((bakeDir(r) / kOldBake).string(), 2 * kBar, kOld);
}

/// An element of audio tracks, one per value in @p values, each playing its own
/// clip of that value for @p bars bars; at @p bpm (0: never given one).
AudioSong audioElement(Rig& r, const std::string& ref, const std::vector<int16_t>& values, uint16_t bars,
                       float bpm = 120.0f) {
    AudioSong s;
    s.dir = r.platform.root + "/" + ref;
    fs::create_directories(fs::path(s.dir) / AudioSong::clipsDir());
    s.bpm = bpm;
    for (size_t t = 0; t < values.size(); ++t) {
        const std::string file = "t" + std::to_string(t) + ".wav";
        writeClip(s.clipPath(file), bars * kBar, values[t]);
        s.tracks[t].inUse = true;
        s.addClip((int)t, {file, 0, bars});
    }
    return s;
}

void writeAudioElement(Rig& r, const std::string& ref, int16_t value, uint16_t bars) {
    REQUIRE(audioElement(r, ref, {value}, bars).save());
}

/// The song @p name on the card: one block of @p ref on track 0 over @p bars
/// bars, playing @p bake (empty: none yet).
void writeSongWith(Rig& r, const std::string& name, const std::string& ref, uint16_t bars, const std::string& bake) {
    AudioSong s;
    s.dir = r.platform.root + "/" + name;
    fs::create_directories(fs::path(s.dir) / AudioSong::clipsDir());
    s.bpm = 120.0f;
    s.tracks[0].inUse = true;
    SongClip c;
    c.element = ref;
    c.file = bake;
    c.bars = bars;
    c.frames = bake.empty() ? 0 : bars * kBar;
    s.addClip(0, c);
    REQUIRE(s.save());
}

SongClip block(Rig& r, int track = 0, size_t i = 0) {
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    REQUIRE(r.engine->song().tracks[track].clips.size() > i);
    return r.engine->song().tracks[track].clips[i];
}

/// The relink pass and every bake it queued are through.
bool idle(Rig& r) {
    return waitFor([&] { return !r.engine->backgroundBusy(); }, kBakeWaitMs);
}

/// Every sample of the bake @p file is @p value (within rounding).
void bakeIs(Rig& r, const std::string& file, uint32_t frames, int16_t value) {
    const auto pcm = readWavData((bakeDir(r) / file).string());
    REQUIRE(pcm.size() == (size_t)frames * 2);
    for (size_t i : {size_t(0), pcm.size() / 2, pcm.size() - 1}) CHECK(std::abs(pcm[i] - value) <= 1);
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

/// The real card under the engine, watched: the descriptors it holds open at
/// once, at most; a file it cannot open (@c unopenable); and @c fire, run once
/// on the worker the first time it asks about @c target while @c armed. Set
/// both before the engine's worker starts.
struct HookFs final : IFileSystem {
    IFileSystem& base;
    IFileSystem* was;
    std::atomic<int> open{0}, most{0};
    std::string unopenable, target;
    std::function<void()> fire;
    std::atomic<bool> armed{false};
    HookFs() : base(getFileSystem()), was(getPlatformServices().fileSystem) {
        getPlatformServices().setFileSystem(this);
    }
    ~HookFs() override { getPlatformServices().fileSystem = was; }
    bool list(const char* dir, EntryFn each, void* ctx) override { return base.list(dir, each, ctx); }
    FsStat stat(const char* path, FileStat* out) override {
        if (path == target && armed.exchange(false)) fire();
        return base.stat(path, out);
    }
    bool makeDir(const char* path) override { return base.makeDir(path); }
    bool remove(const char* path) override { return base.remove(path); }
    bool removeDir(const char* path) override { return base.removeDir(path); }
    bool rename(const char* from, const char* to) override { return base.rename(from, to); }
    int openRead(const char* path) override {
        if (path == unopenable) return -1;
        const int fd = base.openRead(path);
        if (fd >= 0) {
            const int now = ++open;
            int m = most.load();
            while (now > m && !most.compare_exchange_weak(m, now)) {}
        }
        return fd;
    }
    long read(int fd, void* dst, size_t n) override { return base.read(fd, dst, n); }
    bool seek(int fd, uint32_t offset) override { return base.seek(fd, offset); }
    void close(int fd) override { base.close(fd); --open; }
};

/// The child's recording, as audioElement() names it.
std::string childClip(Rig& r, int track = 0) {
    return r.platform.root + "/" + kChild + "/" + AudioSong::clipsDir() + "/t" + std::to_string(track) + ".wav";
}

/// A bake of the child that cannot be made from the card leaves the block on
/// the old bake, Stale, with a why naming @p what, and no part file.
void keptOld(Rig& r, const std::string& what) {
    CHECK(block(r).file == kOldBake);
    CHECK(r.engine->elementClipState(0, 0) == ElementClipState::Stale);
    CHECK(r.engine->elementClipWhy(0, 0).find(what) != std::string::npos);
    CHECK(bakes(r) == std::set<std::string>{kOldBake});
}

} // namespace

TEST_CASE("disk bake: an element changed on the card is baked from it and its block linked", "[song][element][diskbake]") {
    Rig r;
    writeOldBake(r);
    writeAudioElement(r, kChild, 8000, 2);
    writeSongWith(r, "Parent", kChild, 2, kOldBake);
    writeSongWith(r, "Other", kChild, 2, kOldBake);
    const std::string other = slurp(fs::path(r.platform.root) / "Other" / AudioSong::fileName());

    REQUIRE(r.open("Parent"));
    REQUIRE(idle(r));
    const SongClip first = block(r);
    CHECK(first.file != kOldBake);
    CHECK(first.frames == 2 * kBar);
    CHECK(r.engine->elementClipState(0, 0) == ElementClipState::Fresh);
    CHECK(r.engine->elementClipWhy(0, 0).empty());
    CHECK(r.engine->undoSteps() == 0);
    bakeIs(r, first.file, 2 * kBar, 8000);
    /* The link is a commit: the song on the card plays the new bake too. */
    REQUIRE(waitFor([&] {
        AudioSong saved;
        return saved.load(fs::path(r.platform.root / fs::path("Parent")).string()) == SongLoad::Loaded &&
               saved.tracks[0].clips.size() == 1 && saved.tracks[0].clips[0].file == first.file;
    }));
    /* Only the open song's blocks: another song using it is left as it is. */
    CHECK(slurp(fs::path(r.platform.root) / "Other" / AudioSong::fileName()) == other);

    /* Nothing changed: a pass that links nothing writes nothing. */
    const fs::path parentJson = fs::path(r.platform.root) / "Parent" / AudioSong::fileName();
    const auto written = fs::last_write_time(parentJson);
    REQUIRE(r.open("Parent"));
    REQUIRE(idle(r));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    CHECK(fs::last_write_time(parentJson) == written);
    CHECK(block(r).file == first.file);

    /* The child's recording replaced on the card: another bake, linked on the next open. */
    const std::string clip = r.platform.root + "/" + kChild + "/" + AudioSong::clipsDir() + "/t0.wav";
    writeClip(clip, 2 * kBar, 12000);
    fs::last_write_time(clip, fs::last_write_time(clip) + std::chrono::seconds(10));
    REQUIRE(r.open("Parent"));
    REQUIRE(idle(r));
    const SongClip second = block(r);
    CHECK(second.file != first.file);
    CHECK(r.engine->elementClipState(0, 0) == ElementClipState::Fresh);
    CHECK(r.engine->undoSteps() == 0);
    bakeIs(r, second.file, 2 * kBar, 12000);
}

TEST_CASE("disk bake: a block laid by an edit is linked to the bake already made", "[song][element][diskbake]") {
    Rig r;
    writeAudioElement(r, kChild, 8000, 2);
    writeSongWith(r, "Parent", kChild, 2, "");
    REQUIRE(r.open("Parent"));
    REQUIRE(idle(r));
    const std::string baked = block(r).file;
    REQUIRE_FALSE(baked.empty());
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        SongClip c;
        c.element = kChild;
        c.bar = 4;
        c.bars = 2;
        r.engine->song().addClip(0, c);
    }
    r.engine->commit();
    REQUIRE(waitFor([&] { return block(r, 0, 1).file == baked; }, kBakeWaitMs));
    CHECK(block(r, 0, 1).frames == 2 * kBar);
    CHECK(bakes(r).size() == 1);
    CHECK(r.engine->undoSteps() == 1);   // the block laid; its link is no step
    CHECK(r.engine->elementClipState(0, 1) == ElementClipState::Fresh);
}

TEST_CASE("disk bake: an element with a seq track not frozen keeps its old bake, stale", "[song][element][diskbake]") {
    Rig r;
    writeOldBake(r);
    {
        AudioSong s = audioElement(r, kChild, {8000}, 2);
        REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 0));
        SeqNote n;
        n.pad = 1;
        n.vel = 100;
        s.putSeqTake(1, 0, makeSeqPattern(1, {n}), s.samplesPerBar(kRate));
        REQUIRE(s.save());
    }
    writeSongWith(r, "Parent", kChild, 2, kOldBake);
    REQUIRE(r.open("Parent"));
    REQUIRE(waitFor([&] { return r.engine->elementClipState(0, 0) == ElementClipState::Stale; }, kBakeWaitMs));
    REQUIRE(idle(r));
    CHECK(block(r).file == kOldBake);
    CHECK(segmentsOf(*r.engine, 0) == 1);                    // the old bake still plays
    CHECK(r.engine->elementClipWhy(0, 0).find("not frozen") != std::string::npos);
    CHECK(bakes(r) == std::set<std::string>{kOldBake});
    CHECK(r.engine->undoSteps() == 0);
}

TEST_CASE("disk bake: a bake gone from the card is missing until it is made again", "[song][element][diskbake]") {
    Rig r;
    writeAudioElement(r, kChild, 8000, 2);
    writeSongWith(r, "Parent", kChild, 2, "");
    REQUIRE(r.open("Parent"));
    REQUIRE(idle(r));
    const std::string baked = block(r).file;
    REQUIRE(r.engine->elementClipState(0, 0) == ElementClipState::Fresh);
    REQUIRE(fs::remove(bakeDir(r) / baked));

    r.platform.holdIo = true;                               // the bake waits at its first breath
    REQUIRE(r.open("Parent"));
    CHECK(waitFor([&] { return r.engine->elementClipState(0, 0) == ElementClipState::Missing; }, kBakeWaitMs));
    CHECK(r.engine->backgroundBusy());
    r.platform.holdIo = false;
    REQUIRE(idle(r));
    CHECK(r.engine->elementClipState(0, 0) == ElementClipState::Fresh);
    CHECK(block(r).file == baked);                          // the same element: the same bake
    bakeIs(r, baked, 2 * kBar, 8000);
    CHECK(r.engine->backgroundProgress() == 100);
}

TEST_CASE("disk bake: the queue gives way to PLAY and goes on after STOP", "[song][element][diskbake]") {
    Rig r;
    writeOldBake(r);
    writeAudioElement(r, kChild, 8000, 2);
    writeSongWith(r, "Parent", kChild, 2, kOldBake);
    r.platform.holdIo = true;
    REQUIRE(r.open("Parent"));
    REQUIRE(waitFor([&] { return r.engine->elementClipState(0, 0) == ElementClipState::Baking; }, kBakeWaitMs));
    REQUIRE(r.engine->play(0));
    r.platform.holdIo = false;
    REQUIRE(waitFor([&] { return r.engine->playing(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(bakes(r) == std::set<std::string>{kOldBake});      // nothing written while it plays
    CHECK(r.engine->elementClipState(0, 0) == ElementClipState::Baking);
    CHECK(r.engine->backgroundBusy());
    CHECK(block(r).file == kOldBake);
    const int before = r.platform.waits;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(r.platform.waits - before >= 10);                  // a bake waiting is no reason not to wait
    r.engine->stop();
    REQUIRE(waitFor([&] { return !r.engine->playing(); }));
    REQUIRE(idle(r));
    CHECK(r.engine->elementClipState(0, 0) == ElementClipState::Fresh);
    CHECK(block(r).file != kOldBake);
    bakeIs(r, block(r).file, 2 * kBar, 8000);
}

TEST_CASE("disk bake: a bake made by leaving and one made from the card have one key", "[song][element][diskbake]") {
    Rig r;
    writeAudioElement(r, kGrandchild, 5000, 2);
    {
        /* Never given a tempo: it is played at the song's, 120. */
        AudioSong s = audioElement(r, kChild, {3000}, 2, 0.0f);
        s.tracks[1].inUse = true;
        SongClip c;
        c.element = kGrandchild;
        c.bars = 2;
        s.addClip(1, c);
        REQUIRE(s.save());
    }
    REQUIRE(r.engine->requestOpenElement(kChild));
    REQUIRE(waitFor([&] { return r.engine->openState() == SongOpState::Ready; }));
    REQUIRE(idle(r));
    REQUIRE(r.engine->elementClipState(1, 0) == ElementClipState::Fresh);
    REQUIRE(r.engine->requestBakeElement());
    REQUIRE(waitFor([&] { return r.engine->editState() != SongOpState::Busy; }, kBakeWaitMs));
    REQUIRE(r.engine->editState() == SongOpState::Ready);
    const ElementBake left = r.engine->bakeResult();
    bakeIs(r, left.file, 2 * kBar, 8000);
    const auto made = bakes(r);
    REQUIRE(made.size() == 2);                               // the grandchild's and the child's

    writeSongWith(r, "Parent", kChild, 2, "");
    REQUIRE(r.open("Parent"));
    REQUIRE(idle(r));
    CHECK(block(r).file == left.file);
    CHECK(block(r).frames == left.frames);
    CHECK(bakes(r) == made);                                 // found, not baked again

    const auto pcm = readWavData((bakeDir(r) / left.file).string());
    REQUIRE(fs::remove(bakeDir(r) / left.file));
    REQUIRE(r.open("Parent"));
    REQUIRE(idle(r));
    CHECK(block(r).file == left.file);
    CHECK(readWavData((bakeDir(r) / left.file).string()) == pcm);
}

TEST_CASE("disk bake: four tracks at once are mixed through two descriptors", "[song][element][diskbake]") {
    HookFs counted;
    Rig r;
    REQUIRE(audioElement(r, kChild, {1000, 2000, 3000, 4000}, 2).save());
    writeSongWith(r, "Parent", kChild, 2, "");
    REQUIRE(r.open("Parent"));
    REQUIRE(idle(r));
    REQUIRE(r.engine->elementClipState(0, 0) == ElementClipState::Fresh);
    bakeIs(r, block(r).file, 2 * kBar, 10000);
    CHECK(counted.most.load() <= 2);                         // and the bake's own writer: three
    CHECK(counted.open.load() == 0);
}

TEST_CASE("disk bake: a bake the card cannot take changes nothing and leaves no part file", "[song][element][diskbake]") {
    Rig r;
    writeOldBake(r);
    writeAudioElement(r, kChild, 8000, 2);
    writeSongWith(r, "Parent", kChild, 2, kOldBake);
    REQUIRE(r.open("Parent"));
    REQUIRE(idle(r));
    const std::string baked = block(r).file;
    REQUIRE(baked != kOldBake);
    /* Back as it was, and a folder by the bake's own name where the bake was:
     * the next one cannot be renamed into place. */
    REQUIRE(r.open("Other"));
    writeSongWith(r, "Parent", kChild, 2, kOldBake);
    REQUIRE(fs::remove(bakeDir(r) / baked));
    fs::create_directories(bakeDir(r) / baked / "x");
    REQUIRE(r.open("Parent"));
    REQUIRE(idle(r));
    CHECK(block(r).file == kOldBake);
    CHECK(r.engine->elementClipState(0, 0) == ElementClipState::Stale);
    CHECK_FALSE(r.engine->elementClipWhy(0, 0).empty());
    for (const auto& f : bakes(r)) CHECK(f.find(".part") == std::string::npos);
}

TEST_CASE("disk bake: a new tempo keeps a block on its old bake while its recordings are not fitted", "[song][element][diskbake]") {
    Rig r;
    writeAudioElement(r, kChild, 8000, 2);
    writeSongWith(r, "Parent", kChild, 2, "");
    REQUIRE(r.open("Parent"));
    REQUIRE(idle(r));
    const SongClip before = block(r);
    REQUIRE(r.engine->elementClipState(0, 0) == ElementClipState::Fresh);
    /* Entered at 100 its recording would be stretched to the bars; laid as
     * recorded it would end early in each one, and that is no bake of it. */
    REQUIRE(r.engine->requestTempo(100.0f));
    REQUIRE(waitFor([&] { return r.engine->editState() != SongOpState::Busy; }));
    REQUIRE(waitFor([&] { return r.engine->elementClipState(0, 0) == ElementClipState::Stale; }, kBakeWaitMs));
    REQUIRE(idle(r));
    CHECK(block(r).file == before.file);
    CHECK(block(r).frames == before.frames);
    CHECK(r.engine->elementClipWhy(0, 0).find("fitted") != std::string::npos);
    CHECK(bakes(r).size() == 1);
}

TEST_CASE("disk bake: a frozen seq track is mixed from its freeze until its notes move past it", "[song][element][diskbake]") {
    Rig r;
    writeOldBake(r);
    AudioSong el = audioElement(r, kChild, {8000}, 2);
    REQUIRE(el.addSeqTrack(1, SeqSource::Kit, 0));
    SeqNote n;
    n.pad = 1;
    n.vel = 100;
    el.putSeqTake(1, 0, makeSeqPattern(1, {n}), el.samplesPerBar(kRate), 2);
    SongClip fz;
    fz.file = "f.wav";
    fz.bars = 2;
    el.tracks[1].freeze = {fz};
    el.tracks[1].freezeKey = seq_freeze_key(el, 1, kRate, SceneLookup{});
    writeClip(el.clipPath("f.wav"), 2 * kBar, 4000);
    REQUIRE(el.save());
    writeSongWith(r, "Parent", kChild, 2, kOldBake);
    REQUIRE(r.open("Parent"));
    REQUIRE(idle(r));
    const std::string frozen = block(r).file;
    CHECK(frozen != kOldBake);
    CHECK(r.engine->elementClipState(0, 0) == ElementClipState::Fresh);
    bakeIs(r, frozen, 2 * kBar, 12000);

    /* Edited inside and never left: new notes, the old freeze. */
    n.pad = 2;
    el.putSeqTake(1, 0, makeSeqPattern(1, {n}), el.samplesPerBar(kRate), 2);
    REQUIRE(el.save());
    REQUIRE(r.open("Parent"));
    REQUIRE(idle(r));
    CHECK(block(r).file == frozen);
    CHECK(r.engine->elementClipState(0, 0) == ElementClipState::Stale);
    CHECK(r.engine->elementClipWhy(0, 0).find("older than its notes") != std::string::npos);
    CHECK(bakes(r) == std::set<std::string>{kOldBake, frozen});
}

TEST_CASE("disk bake: an element block laid while a pass links is resolved by a pass of its own", "[song][element][diskbake]") {
    HookFs card;
    Rig r;
    writeAudioElement(r, kChild, 8000, 2);
    writeAudioElement(r, kGrandchild, 5000, 2);
    {
        /* A recording is told apart by its name, size and time: another second. */
        const std::string clip = r.platform.root + "/" + kGrandchild + "/" + AudioSong::clipsDir() + "/t0.wav";
        fs::last_write_time(clip, fs::last_write_time(clip) + std::chrono::seconds(10));
    }
    card.target = (bakeDir(r) / kOldBake).string();
    card.fire = [&r] {
        /* The pass has resolved, and links next: an edit made under the lock and
         * not committed yet -- the window of {lock; edit;} commit(). */
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        SongClip c;
        c.element = kGrandchild;
        c.bar = 4;
        c.bars = 2;
        r.engine->song().addClip(0, c);
    };
    writeSongWith(r, "Parent", kChild, 2, "");
    REQUIRE(r.open("Parent"));
    REQUIRE(idle(r));
    REQUIRE(r.open("Other"));
    writeOldBake(r);
    writeSongWith(r, "Parent", kChild, 2, kOldBake);         // a link for the pass to make
    card.armed = true;
    REQUIRE(r.open("Parent"));
    REQUIRE(waitFor([&] { return !card.armed; }, kBakeWaitMs));
    r.engine->commit();
    REQUIRE(idle(r));
    CHECK(block(r).file != kOldBake);
    const SongClip laid = block(r, 0, 1);
    CHECK(laid.element == kGrandchild);
    CHECK_FALSE(laid.file.empty());
    CHECK(r.engine->elementClipState(0, 1) == ElementClipState::Fresh);
    bakeIs(r, laid.file, 2 * kBar, 5000);
}

TEST_CASE("disk bake: a recording cut short fails the bake and names it", "[song][element][diskbake]") {
    Rig r;
    writeOldBake(r);
    writeAudioElement(r, kChild, 8000, 2);
    fs::resize_file(childClip(r), fs::file_size(childClip(r)) / 2);   // the header still says two bars
    writeSongWith(r, "Parent", kChild, 2, kOldBake);
    REQUIRE(r.open("Parent"));
    REQUIRE(idle(r));
    keptOld(r, "t0.wav");
}

TEST_CASE("disk bake: a recording that cannot be opened fails the bake and names it", "[song][element][diskbake]") {
    HookFs card;
    Rig r;
    card.unopenable = childClip(r);
    writeOldBake(r);
    writeAudioElement(r, kChild, 8000, 2);
    writeSongWith(r, "Parent", kChild, 2, kOldBake);
    REQUIRE(r.open("Parent"));
    REQUIRE(idle(r));
    keptOld(r, "t0.wav");
}

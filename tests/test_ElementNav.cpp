// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>

#include "song_engine_rig.hpp"

#include "crosspad/kit/KitPathUtils.hpp"
#include "crosspad/platform/IFileSystem.hpp"
#include "crosspad/sequencer/SequenceStore.hpp"
#include "crosspad/song/ElementNav.hpp"
#include "crosspad/song/SongFiles.hpp"
#include "crosspad/song/SongScenes.hpp"
#include "crosspad/song/SongSounds.hpp"

namespace {

constexpr uint32_t kBarFrames = 2 * kRate;                  // a bar at 120 BPM
const char* const kA = ".elements/E00000001";
const char* const kB = ".elements/E00000002";
const char* const kC = ".elements/E00000003";

/// Nothing runs on the board's ElementNav between cases.
struct NavGuard {
    NavGuard() { elementNav().reset(); }
    ~NavGuard() { elementNav().reset(); }
};

/// An element written onto the card: @p bars bars of one constant audio track
/// at @p bpm, and a block of each element in @p uses on the tracks after it.
void writeAudioElement(Rig& r, const std::string& ref, const std::string& title, uint32_t bars,
                       float bpm, const std::vector<std::string>& uses = {}) {
    AudioSong s;
    s.dir = r.platform.root + "/" + ref;
    fs::create_directories(fs::path(s.dir) / AudioSong::clipsDir());
    s.title = title;
    s.bpm = bpm;
    if (bars) {
        writeClip((fs::path(s.dir) / AudioSong::clipsDir() / "a.wav").string(), bars * kBarFrames, 8192);
        s.tracks[0].inUse = true;
        s.addClip(0, {"a.wav", 0, (uint16_t)bars});
    }
    for (size_t i = 0; i < uses.size(); ++i) {
        SongClip c;
        c.element = uses[i];
        c.bars = 2;
        s.tracks[1 + i].inUse = true;
        s.addClip((int)(1 + i), c);
    }
    REQUIRE(s.save());
}

/// "Demo" open at 120 BPM with a block of @p ref on track 1 from bar 0.
void openDemo(Rig& r, const std::string& ref) {
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.bpm = 120.0f;
        s.tracks[1].inUse = true;
        SongClip c;
        c.element = ref;
        c.bars = 4;
        s.addClip(1, c);
    }
    r.engine->commit();
    /* The block laid, the background bakes what it can of it from the card and
     * links it: every job below starts from a song at rest. */
    REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, 10000));
}

ElementNav::Result runNav() {
    ElementNav::Result res;
    waitFor([&] { res = elementNav().poll(); return res.done; }, 20000);
    return res;
}

std::string openName(Rig& r) {
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    return r.engine->song().name;
}

SongClip clipOf(Rig& r, int track) {
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    return r.engine->song().tracks[track].clips.at(0);
}

bool listElements(Rig& r) {
    const uint32_t gen = r.engine->listGeneration();
    return r.engine->requestElements() && waitFor([&] { return r.engine->listGeneration() != gen; });
}

fs::path bakeOf(Rig& r, const std::string& file) {
    return fs::path(r.platform.root) / AudioSong::bakeDir() / file;
}

/// Once the background is through: the block on @p track plays its element's
/// bake as the card has it, and that is the only bake there is -- nothing of
/// the song open was baked into it.
void onlyCardBake(Rig& r, int track) {
    REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, 10000));
    const SongClip c = clipOf(r, track);
    CHECK(r.engine->elementClipState((uint8_t)track, 0) == ElementClipState::Fresh);
    std::vector<std::string> bakes;
    for (const auto& e : fs::directory_iterator(fs::path(r.platform.root) / AudioSong::bakeDir())) {
        if (!cacheBook(e.path())) bakes.push_back(e.path().filename().string());
    }
    CHECK(bakes == std::vector<std::string>{c.file});
}

/// The board's SongSounds over a host whose kit loads end when the test says.
struct FakeSoundsHost final : ISongSoundsHost {
    std::atomic<bool> kitsLoaded{true};
    std::string cardRoot() override { return {}; }
    bool kitsSettled() override { return kitsLoaded; }
    void loadKit(int) override {}
    void unloadKit() override {}
    bool instrumentOwnsEngine() override { return false; }
    const char* foregroundApp() override { return nullptr; }
    void warn(const char*) override {}
    void log(bool, const char*) override {}
};

struct SoundsGuard {
    FakeSoundsHost host;
    SongSounds sounds{host};
    SongSounds* before;
    SoundsGuard() : before(getPlatformServices().songSounds) { getPlatformServices().songSounds = &sounds; }
    ~SoundsGuard() { getPlatformServices().songSounds = before; }
};

/// Polls the job, rendering as the board would, until it reaches @p stage.
bool pollTo(Rig& r, const char* stage) {
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    return waitFor([&] {
        r.block(out, bus);
        elementNav().poll();
        return std::string(elementNav().stage()) == stage;
    }, 10000);
}

/// The card under the engine with one file held shut: opening @c path waits
/// while @c held, at most kGateMs, so a bake that reads it waits for the test.
/// Install before the Rig; release() before the Rig goes (its worker is joined).
struct GateFs final : IFileSystem {
    static constexpr int kGateMs = 10000;
    IFileSystem& base;
    IFileSystem* was;
    std::mutex m;
    std::string path;
    std::atomic<bool> held{false};
    GateFs() : base(getFileSystem()), was(getPlatformServices().fileSystem) {
        getPlatformServices().setFileSystem(this);
    }
    ~GateFs() override { getPlatformServices().fileSystem = was; }
    void hold(const std::string& p) {
        std::lock_guard<std::mutex> l(m);
        path = p;
        held = true;
    }
    void release() { held = false; }
    bool list(const char* dir, EntryFn each, void* ctx) override { return base.list(dir, each, ctx); }
    FsStat stat(const char* p, FileStat* out) override { return base.stat(p, out); }
    bool makeDir(const char* p) override { return base.makeDir(p); }
    bool remove(const char* p) override { return base.remove(p); }
    bool removeDir(const char* p) override { return base.removeDir(p); }
    bool rename(const char* from, const char* to) override { return base.rename(from, to); }
    int openRead(const char* p) override {
        bool gated;
        {
            std::lock_guard<std::mutex> l(m);
            gated = held && path == p;
        }
        if (gated) waitFor([&] { return !held; }, kGateMs);
        return base.openRead(p);
    }
    long read(int fd, void* dst, size_t n) override { return base.read(fd, dst, n); }
    bool seek(int fd, uint32_t offset) override { return base.seek(fd, offset); }
    void close(int fd) override { base.close(fd); }
};

/// Lets the gate go as the case ends, before the Rig joins the worker.
struct GateRelease {
    GateFs& gate;
    ~GateRelease() { gate.release(); }
};

} // namespace

TEST_CASE("element nav: enter opens the element, leave opens the parent and its block plays the element's bake", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    openDemo(r, kA);
    REQUIRE(elementNav().enter(1, 0));
    CHECK(elementNav().busy());
    const auto in = runNav();
    REQUIRE(in.done);
    REQUIRE(in.ok);
    CHECK(in.job == ElementNav::Job::Enter);
    CHECK(elementNav().depth() == 1);
    CHECK(elementNav().path()[0].open == "Demo");
    CHECK(elementNav().path()[1].open == kA);
    CHECK(elementNav().path()[1].title == "Intro");
    CHECK(openName(r) == kA);
    CHECK(r.platform.current == "Demo");              // never the project a boot opens

    REQUIRE(elementNav().leave());
    const auto out = runNav();
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    CHECK(out.job == ElementNav::Job::Leave);
    CHECK_FALSE(elementNav().busy());
    CHECK(elementNav().depth() == 0);
    CHECK(openName(r) == "Demo");
    onlyCardBake(r, 1);
    const SongClip c = clipOf(r, 1);
    CHECK_FALSE(c.file.empty());
    CHECK(c.frames == 2 * kBarFrames);
    CHECK(fs::exists(bakeOf(r, c.file)));
    CHECK(segmentsOf(*r.engine, 1) > 0);
}

TEST_CASE("element nav: a leave with only its audio changed is back in the parent before the bake", "[element][nav]") {
    GateFs gate;
    Rig r;
    GateRelease letGo{gate};
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    openDemo(r, kA);
    const SongClip was = clipOf(r, 1);
    REQUIRE_FALSE(was.file.empty());
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[0].volume = 0.5f;
    }
    r.engine->commit();

    /* Its recording held shut: no bake of it can be mixed until the test says. */
    gate.hold(r.platform.root + "/" + kA + "/" + AudioSong::clipsDir() + "/a.wav");
    REQUIRE(elementNav().leave());
    const auto out = runNav();
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    CHECK(openName(r) == "Demo");
    CHECK(elementNav().depth() == 0);
    CHECK(elementNav().changed());
    CHECK(clipOf(r, 1).file == was.file);                  // the old bake plays meanwhile
    CHECK(clipOf(r, 1).frames == was.frames);
    REQUIRE(waitFor([&] { return r.engine->elementClipState(1, 0) == ElementClipState::Baking; }, 10000));
    for (const auto& e : fs::directory_iterator(fs::path(r.platform.root) / AudioSong::bakeDir())) {
        const std::string f = e.path().filename().string();
        CHECK((f == was.file || f == "usage.json" || f.find(".part") != std::string::npos));
    }

    gate.release();
    REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, 10000));
    const SongClip now = clipOf(r, 1);
    CHECK(now.file != was.file);
    CHECK(now.frames == 2 * kBarFrames);
    CHECK(r.engine->elementClipState(1, 0) == ElementClipState::Fresh);
    const auto pcm = readWavData(bakeOf(r, now.file).string());
    REQUIRE(pcm.size() == (size_t)2 * kBarFrames * 2);
    CHECK(std::abs(pcm[pcm.size() / 2] - 4096) <= 1);
}

TEST_CASE("element nav: a block laid and entered at once, while the background bakes it", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kB, "Hat", 2, 120.0f);
    writeAudioElement(r, kA, "Groove", 2, 120.0f, {kB});
    REQUIRE(r.open("Demo"));
    for (int round = 0; round < 3; ++round) {
        {
            /* No wait for the pass the commit asks for: the enter goes in beside it. */
            std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
            AudioSong& s = r.engine->song();
            s.bpm = 120.0f;
            s.tracks[1].inUse = true;
            s.tracks[1].clips.clear();
            SongClip c;
            c.element = kA;
            c.bars = 4;
            s.addClip(1, c);
        }
        r.engine->commit();
        REQUIRE(elementNav().enter(1, 0));
        const auto in = runNav();
        INFO("round " << round << " error: " << in.error);
        REQUIRE(in.ok);
        REQUIRE(elementNav().leave());
        const auto out = runNav();
        INFO("error: " << out.error);
        REQUIRE(out.ok);
        REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, 10000));
        CHECK_FALSE(clipOf(r, 1).file.empty());
        CHECK(r.engine->elementClipState(1, 0) == ElementClipState::Fresh);
    }
    /* B's bake and A's: the same two whoever made them. */
    size_t bakes = 0;
    for (const auto& e : fs::directory_iterator(fs::path(r.platform.root) / AudioSong::bakeDir())) {
        if (!cacheBook(e.path())) ++bakes;
    }
    CHECK(bakes == 2);
}

TEST_CASE("element nav: leaving unchanged finds the bake in the cache", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    openDemo(r, kA);
    const std::string baked = clipOf(r, 1).file;
    for (int round = 0; round < 2; ++round) {
        REQUIRE(elementNav().enter(1, 0));
        REQUIRE(runNav().ok);
        REQUIRE(elementNav().leave());
        REQUIRE(runNav().ok);
        /* The first time too: the background baked it when the block was laid, by the same key. */
        onlyCardBake(r, 1);
        CHECK(clipOf(r, 1).file == baked);
    }
}

TEST_CASE("element nav: an edit two levels down reaches the song", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kB, "Hat", 2, 120.0f);
    writeAudioElement(r, kA, "Groove", 0, 120.0f, {kB});
    {
        /* A's block of B goes on track 1; give A a length so it has a bar to bake. */
        AudioSong a;
        REQUIRE(a.load(r.platform.root + "/" + kA) == SongLoad::Loaded);
        a.length = 2;
        REQUIRE(a.save());
    }
    openDemo(r, kA);

    auto cycle = [&](float volume, std::string& fileInA, std::string& fileInDemo) {
        REQUIRE(elementNav().enter(1, 0));
        REQUIRE(runNav().ok);
        REQUIRE(elementNav().enter(1, 0));
        const auto step = runNav();
        INFO("error: " << step.error);
        REQUIRE(step.ok);
        REQUIRE(elementNav().depth() == 2);
        {
            std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
            r.engine->song().tracks[0].volume = volume;
        }
        r.engine->commit();
        REQUIRE(elementNav().leave());
        const auto inner = runNav();
        INFO("error: " << inner.error);
        REQUIRE(inner.ok);
        REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, 10000));
        fileInA = clipOf(r, 1).file;
        REQUIRE(elementNav().leave());
        REQUIRE(runNav().ok);
        REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, 10000));
        fileInDemo = clipOf(r, 1).file;
    };
    std::string a1, d1, a2, d2;
    cycle(1.0f, a1, d1);
    cycle(0.5f, a2, d2);
    CHECK_FALSE(a1.empty());
    CHECK_FALSE(d1.empty());
    CHECK(a2 != a1);
    CHECK(d2 != d1);
}

TEST_CASE("element nav: leave while playing stops and opens the parent", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    openDemo(r, kA);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    REQUIRE(r.engine->play(0));
    REQUIRE(waitFor([&] { return r.engine->playing(); }));
    REQUIRE(elementNav().leave());
    CHECK(std::string(elementNav().stage()) == "stopping");
    const auto out = runNav();
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    CHECK_FALSE(r.engine->playing());
    CHECK(openName(r) == "Demo");
    onlyCardBake(r, 1);
}

TEST_CASE("element nav: a bake the card cannot take after a leave leaves the block as it was", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    openDemo(r, kA);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    {
        fs::remove_all(fs::path(r.platform.root) / AudioSong::bakeDir());
        std::FILE* f = std::fopen((fs::path(r.platform.root) / AudioSong::bakeDir()).string().c_str(), "wb");
        REQUIRE(f);
        std::fclose(f);                      // .bake is a file: nothing can be made in it
    }
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[0].volume = 0.5f;        // a bake of its own, which cannot be written
    }
    r.engine->commit();
    const SongClip was = [&] {
        AudioSong demo;
        REQUIRE(demo.load(r.platform.root + "/Demo") == SongLoad::Loaded);
        return demo.tracks[1].clips.at(0);
    }();
    REQUIRE(elementNav().leave());
    const auto out = runNav();
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    CHECK(elementNav().depth() == 0);
    CHECK(openName(r) == "Demo");
    REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, 10000));
    CHECK(clipOf(r, 1).file == was.file);
    CHECK(r.engine->elementClipState(1, 0) == ElementClipState::Missing);
    CHECK_FALSE(r.engine->elementClipWhy(1, 0).empty());
}

TEST_CASE("element nav: a leave that needs the engine and whose bake fails keeps you in the element", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    openDemo(r, kA);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    {
        /* A stretched recording whose length the song does not hold: only the
         * engine can tell it is fitted, so the leave bakes before it goes up. */
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[0].volume = 0.5f;
        r.engine->song().tracks[0].stretch = StretchMode::KeepPitch;
        REQUIRE(r.engine->song().tracks[0].clips[0].frames == 0);
    }
    r.engine->commit();
    REQUIRE(waitFor([&] { return r.engine->stretchWaiting() == 0 && !r.engine->backgroundBusy(); }, 10000));
    {
        fs::remove_all(fs::path(r.platform.root) / AudioSong::bakeDir());
        std::FILE* f = std::fopen((fs::path(r.platform.root) / AudioSong::bakeDir()).string().c_str(), "wb");
        REQUIRE(f);
        std::fclose(f);                      // .bake is a file: nothing can be made in it
    }
    REQUIRE(elementNav().leave());
    const auto out = runNav();
    CHECK(out.done);
    CHECK_FALSE(out.ok);
    CHECK_FALSE(out.error.empty());
    CHECK(elementNav().lastError() == out.error);
    CHECK_FALSE(elementNav().busy());
    CHECK(elementNav().depth() == 1);
    CHECK(openName(r) == kA);
    CHECK(out.error.find("bake") != std::string::npos);    // the engine's bake, not the background's
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    CHECK(r.engine->song().tracks[0].volume == 0.5f);      // the edit is still there
}

TEST_CASE("element nav: a deleted bake comes back after enter and leave", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    openDemo(r, kA);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    REQUIRE(elementNav().leave());
    REQUIRE(runNav().ok);
    const std::string file = clipOf(r, 1).file;
    REQUIRE(fs::remove(bakeOf(r, file)));
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    REQUIRE(elementNav().leave());
    REQUIRE(runNav().ok);
    onlyCardBake(r, 1);
    CHECK(clipOf(r, 1).file == file);
    CHECK(fs::exists(bakeOf(r, file)));
}

TEST_CASE("element nav: an element cannot hold itself", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kB, "Hat", 2, 120.0f);
    writeAudioElement(r, kA, "Groove", 2, 120.0f, {kB});    // A uses B
    writeAudioElement(r, kC, "Fill", 2, 120.0f, {kA});      // C uses A
    openDemo(r, kA);
    REQUIRE(listElements(r));
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    CHECK_FALSE(elementNav().insertExisting(2, 4, kA));
    CHECK(elementNav().lastError().find("cannot hold itself") != std::string::npos);
    CHECK_FALSE(elementNav().insertExisting(2, 4, kC));
    CHECK(elementNav().lastError().find("cannot hold itself") != std::string::npos);
    CHECK_FALSE(elementNav().busy());
    REQUIRE(elementNav().enter(1, 0));                       // into B
    const auto step = runNav();
    INFO("error: " << step.error);
    REQUIRE(step.ok);
    CHECK_FALSE(elementNav().insertExisting(2, 4, kA));
    CHECK(elementNav().lastError().find("cannot hold itself") != std::string::npos);
    CHECK_FALSE(elementNav().busy());
    CHECK(elementNav().depth() == 2);
}

TEST_CASE("element nav: an existing element is laid and baked at the song's tempo", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kB, "Hat", 2, 120.0f);
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().bpm = 120.0f;
    }
    r.engine->commit();
    REQUIRE(listElements(r));
    REQUIRE(elementNav().insertExisting(2, 0, kB));          // track 2 is unused: it becomes audio
    CHECK(elementNav().busy());
    const auto out = runNav();
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    CHECK(out.job == ElementNav::Job::Insert);
    CHECK(elementNav().depth() == 0);
    CHECK(openName(r) == "Demo");
    const SongClip c = clipOf(r, 2);
    CHECK(c.element == kB);
    CHECK(c.bars == 2);
    CHECK(c.file == r.engine->bakeResult().file);
    CHECK(c.frames == 2 * kBarFrames);
}

TEST_CASE("element nav: a seq track takes no element", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        REQUIRE(r.engine->song().addSeqTrack(0, SeqSource::Kit, 0));
    }
    r.engine->commit();
    CHECK_FALSE(elementNav().insertNew(0, 0, "Nope"));
    CHECK_FALSE(elementNav().lastError().empty());
    CHECK_FALSE(elementNav().busy());
}

TEST_CASE("element nav: a new element is entered with its title and the song's tempo", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().bpm = 100.0f;
    }
    r.engine->commit();
    REQUIRE(elementNav().insertNew(2, 4, "Verse"));
    const auto out = runNav();
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    CHECK(out.job == ElementNav::Job::Insert);
    REQUIRE(elementNav().depth() == 1);
    CHECK(elementNav().path()[1].title == "Verse");
    CHECK(elementNav().path()[1].track == 2);
    CHECK(elementNav().path()[1].bar == 4);
    CHECK(is_element_ref(elementNav().path()[1].open));
    const std::string ref = elementNav().path()[1].open;
    CHECK(openName(r) == ref);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        const AudioSong& s = r.engine->song();
        CHECK(s.title == "Verse");
        CHECK(s.length == ElementNav::kNewElementBars);
        CHECK(s.bpm == 100.0f);
    }
    REQUIRE(elementNav().leaveTo(0));
    REQUIRE(runNav().ok);
    REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, 10000));
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const SongTrack& t = r.engine->song().tracks[2];
    REQUIRE(t.clips.size() == 1);
    CHECK(t.clips[0].bar == 4);
    CHECK(t.clips[0].bars == ElementNav::kNewElementBars);
    CHECK(t.clips[0].element == ref);
    CHECK_FALSE(t.clips[0].file.empty());
}

TEST_CASE("element nav: entering at another tempo fits the element's recordings", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Slow", 2, 90.0f);
    openDemo(r, kA);
    REQUIRE(elementNav().enter(1, 0));
    const auto in = runNav();
    INFO("error: " << in.error);
    REQUIRE(in.ok);
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const AudioSong& s = r.engine->song();
    CHECK(s.tracks[0].stretch == StretchMode::KeepPitch);
    CHECK(s.bpm == 120.0f);
}

TEST_CASE("element nav: an element laid at another tempo is baked at the song's", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kB, "Hat", 2, 90.0f);
    REQUIRE(r.engine->requestOpenElement(kB));
    REQUIRE(waitFor([&] { return r.engine->openState() == SongOpState::Ready; }));
    REQUIRE(r.engine->requestBakeElement());                 // the element as it was made, at 90
    REQUIRE(waitFor([&] { return r.engine->editState() != SongOpState::Busy; }, 10000));
    REQUIRE(r.engine->editState() == SongOpState::Ready);
    const ElementBake at90 = r.engine->bakeResult();
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().bpm = 120.0f;
    }
    r.engine->commit();
    REQUIRE(listElements(r));
    REQUIRE(elementNav().insertExisting(2, 0, kB));
    const auto out = runNav();
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    const SongClip c = clipOf(r, 2);
    CHECK_FALSE(c.file.empty());
    CHECK(c.file != at90.file);
    CHECK(c.frames == 2 * kBarFrames);                       // two bars at 120, not at 90
    CHECK(at90.frames != c.frames);
    CHECK(fs::exists(bakeOf(r, c.file)));
}

TEST_CASE("element nav: leaving freezes a live seq track before the bake", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Beat", 2, 120.0f);
    openDemo(r, kA);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 0));
        SeqNote n;
        n.pad = 1; n.vel = 100;
        s.putSeqTake(1, 0, makeSeqPattern(1, {n}), s.samplesPerBar(kRate));
        s.armed = -1;
    }
    r.engine->commit();
    REQUIRE(r.engine->freezeState(1) != FreezeState::Frozen);
    REQUIRE(elementNav().leave());
    /* The freeze renders off the audio clock: the blocks are the test's. */
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.25f);
    ElementNav::Result res;
    REQUIRE(waitFor([&] {
        r.block(out, bus);
        res = elementNav().poll();
        return res.done;
    }, 20000));
    INFO("error: " << res.error);
    REQUIRE(res.ok);
    CHECK(openName(r) == "Demo");
    const SongClip c = clipOf(r, 1);
    CHECK(c.file == r.engine->bakeResult().file);
    CHECK_FALSE(c.file.empty());
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    CHECK(r.engine->freezeState(1) == FreezeState::Frozen);
}

TEST_CASE("element nav: a leave whose audio is never fitted gives up and stays in the element", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Slow", 2, 120.0f);
    openDemo(r, kA);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    r.platform.storage = false;                              // no card: nothing is ever fitted
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[0].stretch = StretchMode::KeepPitch;
        r.engine->song().tracks[0].clips[0].bars = 3;        // a length no copy exists for
    }
    r.engine->commit();
    REQUIRE(r.engine->stretchWaiting() > 0);
    REQUIRE(elementNav().leave());
    const auto out = runNav();
    CHECK(out.done);
    CHECK_FALSE(out.ok);
    CHECK_FALSE(out.error.empty());
    CHECK_FALSE(elementNav().busy());
    CHECK(elementNav().depth() == 1);
    CHECK(openName(r) == kA);
}

TEST_CASE("element nav: a song never given a tempo is entered at the default one", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kB, "Hat", 2, 90.0f);
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().bpm = 0.0f;
    }
    r.engine->commit();
    REQUIRE(listElements(r));
    REQUIRE(elementNav().insertExisting(2, 0, kB));
    const auto out = runNav();
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    const SongClip c = clipOf(r, 2);
    CHECK(c.element == kB);
    CHECK_FALSE(c.file.empty());
    CHECK(c.frames == 2 * kBarFrames);                       // two bars at 120, not at 90
    CHECK(fs::exists(bakeOf(r, c.file)));
}

TEST_CASE("element nav: a song opened behind its back starts the breadcrumb over", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    openDemo(r, kA);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    REQUIRE(elementNav().depth() == 1);

    SECTION("the song itself, from the kit browser's SONGS") {
        REQUIRE(r.open("Demo"));
        CHECK(elementNav().depth() == 0);
        CHECK(elementNav().path()[0].open == "Demo");
        /* A leave from the element it last knew would bake the song into the
         * element's own blocks. */
        CHECK_FALSE(elementNav().leave());
        CHECK_FALSE(elementNav().busy());
        onlyCardBake(r, 1);
    }
    SECTION("another song") {
        REQUIRE(r.open("Other"));
        CHECK(elementNav().depth() == 0);
        CHECK(elementNav().path()[0].open == "Other");
        CHECK_FALSE(elementNav().leave());
    }
}

TEST_CASE("element nav: a song opened during a leave is neither baked nor linked", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    SoundsGuard sounds;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    openDemo(r, kA);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    sounds.host.kitsLoaded = false;                          // the job waits for them: the window to open in
    REQUIRE(elementNav().leave());
    REQUIRE(pollTo(r, "loading"));
    REQUIRE(r.open("Demo"));
    sounds.host.kitsLoaded = true;
    const auto out = runNav();
    CHECK(out.done);
    CHECK_FALSE(out.ok);
    CHECK(out.error.find("another song") != std::string::npos);
    CHECK(openName(r) == "Demo");
    onlyCardBake(r, 1);
    CHECK(elementNav().depth() == 0);
}

TEST_CASE("element nav: a tempo turned inside the element is put back before the bake", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    openDemo(r, kA);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    REQUIRE(r.engine->requestTempo(100.0f));                 // the top bar's BPM, turned inside
    REQUIRE(waitFor([&] { return r.engine->editState() != SongOpState::Busy; }, 10000));
    REQUIRE(r.engine->editState() == SongOpState::Ready);
    REQUIRE(elementNav().leave());
    const auto out = runNav();
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    CHECK(openName(r) == "Demo");
    REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, 10000));
    CHECK(r.engine->elementClipState(1, 0) == ElementClipState::Fresh);
    CHECK(clipOf(r, 1).frames == 2 * kBarFrames);            // two bars at the song's 120, not at 100
}

TEST_CASE("element nav: nothing is frozen before the level's sounds are loaded", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    SoundsGuard sounds;
    writeAudioElement(r, kA, "Beat", 2, 120.0f);
    openDemo(r, kA);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 0));
        SeqNote n;
        n.pad = 1; n.vel = 100;
        s.putSeqTake(1, 0, makeSeqPattern(1, {n}), s.samplesPerBar(kRate));
        s.armed = -1;
    }
    r.engine->commit();
    sounds.sounds.songOpened();                              // a kit still to come, as after an open
    REQUIRE(elementNav().leave());
    REQUIRE(pollTo(r, "loading"));
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.25f);
    for (int i = 0; i < 100; ++i) {
        r.block(out, bus);
        REQUIRE_FALSE(elementNav().poll().done);
    }
    CHECK(std::string(elementNav().stage()) == "loading");
    CHECK(r.engine->bounceStatus().state != BounceState::Running);
    CHECK(r.engine->freezeState(1) != FreezeState::Frozen);

    SECTION("loaded, the leave goes on") {
        sounds.sounds.step();                                // no kit manager: the step has nothing to bring
        REQUIRE(sounds.sounds.settled());
        ElementNav::Result res;
        REQUIRE(waitFor([&] {
            r.block(out, bus);
            res = elementNav().poll();
            return res.done;
        }, 20000));
        INFO("error: " << res.error);
        REQUIRE(res.ok);
        CHECK(openName(r) == "Demo");
        CHECK_FALSE(clipOf(r, 1).file.empty());
    }
    SECTION("never loaded, the leave is given up in the element") {
        const auto res = runNav();
        CHECK(res.done);
        CHECK_FALSE(res.ok);
        CHECK(res.error.find("did not load") != std::string::npos);
        CHECK(elementNav().depth() == 1);
        CHECK(openName(r) == kA);
    }
}

// ── an element made from a region, from a song ────────────────────────────

namespace {

/// The job run to its end, rendering as the board would: a leave freezes seq tracks off the audio clock.
ElementNav::Result runRendering(Rig& r) {
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    ElementNav::Result res;
    waitFor([&] { r.block(out, bus); res = elementNav().poll(); return res.done; }, 20000);
    return res;
}

/// "Demo" with a 4-bar audio clip on track 0 and, on track 1, a seq block of one note.
void regionDemo(Rig& r, bool audio = true) {
    REQUIRE(r.open("Demo"));
    while (r.seq.sceneCount() < 2) REQUIRE(r.seq.addScene());
    writeClip((r.clipsDir("Demo") / "a.wav").string(), 4 * kBarFrames, 8192);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.bpm = 120.0f;
        if (audio) {
            s.tracks[0].inUse = true;
            s.addClip(0, {"a.wav", 0, 4});
        } else {
            REQUIRE(s.addSeqTrack(0, SeqSource::Pitch, 0));
        }
        REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 1));
        SeqNote n;
        n.pad = 5; n.vel = 100; n.tick = AudioSong::kSeqPpq;
        s.putSeqTake(1, 0, makeSeqPattern(4, {n}), s.samplesPerBar(kRate));
        s.armed = -1;
    }
    r.engine->commit();
    REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, 10000));
}

} // namespace

TEST_CASE("element nav: a region becomes an element, one block in its place, baked and linked", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    regionDemo(r);
    REQUIRE(elementNav().makeFromRegion(1, 3, "PART ONE"));
    CHECK(elementNav().busy());
    const auto res = runRendering(r);
    INFO("error: " << res.error);
    REQUIRE(res.ok);
    CHECK(res.job == ElementNav::Job::Make);
    CHECK(elementNav().depth() == 0);
    CHECK(openName(r) == "Demo");
    std::string ref;
    SongClip block;
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        const AudioSong& s = r.engine->song();
        /* Track 0 keeps blocks outside the region, so the element goes on the free track 2. */
        int elements = 0;
        for (int t = 0; t < AudioSong::kTracks; ++t) {
            for (const SongClip& c : s.tracks[t].clips) {
                if (c.isElement()) { ++elements; block = c; ref = c.element; CHECK(t == 2); }
                else CHECK((c.bar + c.bars <= 1 || c.bar >= 3));        // the region is empty of anything else
            }
        }
        CHECK(elements == 1);
        CHECK(s.tracks[0].clips.size() == 2);
        CHECK(s.tracks[0].volume == 1.0f);
    }
    REQUIRE(is_element_ref(ref));
    CHECK(block.bar == 1);
    CHECK(block.bars == 2);
    CHECK(block.file == r.engine->bakeResult().file);
    CHECK(block.frames == 2 * kBarFrames);
    const auto pcm = readWavData(bakeOf(r, block.file).string());
    REQUIRE(pcm.size() == (size_t)2 * kBarFrames * 2);
    for (size_t i : {size_t(0), pcm.size() / 2, pcm.size() - 2}) CHECK(std::abs(pcm[i] - 8192) <= 1);   // the audio the region held
    AudioSong e;
    REQUIRE(e.load(r.platform.root + "/" + ref) == SongLoad::Loaded);
    CHECK(e.title == "PART ONE");
    CHECK(e.length == 2);                    // the region's bars, not a new element's
    REQUIRE(e.tracks[1].clips.size() == 1);
    REQUIRE(e.tracks[1].clips[0].notes);
    CHECK(e.tracks[1].clips[0].notes->notes.size() == 1);
    CHECK(e.tracks[1].clips[0].notes->notes[0].pad == 5);
    CHECK(e.tracks[1].clips[0].notes->notes[0].tick == AudioSong::kSeqPpq);   // the notes are the block's, and the element starts a bar into them
    CHECK(e.tracks[1].clips[0].from == 1);
    size_t at = 0;
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        for (const SongClip& c : r.engine->song().tracks[2].clips) { if (c.isElement()) break; ++at; }
    }
    CHECK(r.engine->elementClipState(2, at) == ElementClipState::Fresh);
}

TEST_CASE("element nav: a region over seq tracks only goes on the lowest free track", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    regionDemo(r, false);
    REQUIRE(elementNav().makeFromRegion(0, 2));
    const auto res = runRendering(r);
    INFO("error: " << res.error);
    REQUIRE(res.ok);
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const AudioSong& s = r.engine->song();
    REQUIRE(s.tracks[2].clips.size() == 1);
    CHECK(s.tracks[2].kind == TrackKind::Audio);
    CHECK(s.tracks[2].clips[0].isElement());
}

TEST_CASE("element nav: a region over seq tracks with no free track is refused and changes nothing", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.bpm = 120.0f;
        for (int t = 0; t < 3; ++t) {
            REQUIRE(s.addSeqTrack(t, t == 1 ? SeqSource::Pitch : SeqSource::Kit, (uint8_t)t, t == 2 ? "k.json" : ""));
            s.putSeqTake(t, 0, makeSeqPattern(2, {SeqNote{}}), s.samplesPerBar(kRate));
        }
        s.tracks[3].inUse = true;
        s.addClip(3, {"a.wav", 8, 2});               // an audio track the region does not touch
    }
    r.engine->commit();
    CHECK_FALSE(elementNav().makeFromRegion(0, 2));
    CHECK(elementNav().lastError().find("needs a free track, or an audio track with nothing outside the region") != std::string::npos);
    CHECK_FALSE(elementNav().busy());
    CHECK(listElements(r));
    CHECK(r.engine->elements().empty());
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    CHECK(r.engine->song().tracks[3].clips.size() == 1);
    CHECK(r.engine->song().tracks[0].clips.size() == 1);
}

TEST_CASE("element nav: a song is copied into the library and laid as an element", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    {
        AudioSong s;
        s.dir = r.platform.root + "/Src";
        fs::create_directories(fs::path(s.dir) / AudioSong::clipsDir());
        writeClip((fs::path(s.dir) / AudioSong::clipsDir() / "a.wav").string(), 2 * kBarFrames, 8192);
        s.bpm = 120.0f;
        s.tracks[0].inUse = true;
        s.addClip(0, {"a.wav", 0, 2});
        REQUIRE(s.save());
    }
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().bpm = 120.0f;
    }
    r.engine->commit();
    CHECK_FALSE(elementNav().insertSong(0, 0, "../Src"));
    REQUIRE(elementNav().insertSong(1, 2, "Src"));
    const auto res = runRendering(r);
    INFO("error: " << res.error);
    REQUIRE(res.ok);
    CHECK(res.job == ElementNav::Job::Insert);
    const SongClip c = clipOf(r, 1);
    REQUIRE(is_element_ref(c.element));
    CHECK(c.bar == 2);
    CHECK(c.bars == 2);
    CHECK(c.file == r.engine->bakeResult().file);
    CHECK(fs::exists(fs::path(r.platform.root) / c.element / "clips" / "a.wav"));
    fs::rename(fs::path(r.platform.root) / "Src", fs::path(r.platform.root) / "Gone");   // a rename breaks nothing
    CHECK(fs::exists(fs::path(r.platform.root) / c.element / "clips" / "a.wav"));
    CHECK(fs::exists(bakeOf(r, c.file)));
}

TEST_CASE("element nav: a region that cannot be copied changes nothing in the song", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    regionDemo(r);
    fs::remove(r.clipsDir("Demo") / "a.wav");
    REQUIRE(elementNav().makeFromRegion(1, 3));
    const auto res = runRendering(r);
    CHECK_FALSE(res.ok);
    CHECK(res.job == ElementNav::Job::Make);
    CHECK_FALSE(res.error.empty());
    CHECK_FALSE(elementNav().busy());
    CHECK(openName(r) == "Demo");
    const fs::path library = fs::path(r.platform.root) / AudioSong::elementsDir();
    CHECK((!fs::exists(library) || fs::is_empty(library)));          // the folder that was started is gone
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    const AudioSong& s = r.engine->song();
    REQUIRE(s.tracks[0].clips.size() == 1);
    CHECK_FALSE(s.tracks[0].clips[0].isElement());
    CHECK(s.tracks[0].clips[0].bars == 4);
}

TEST_CASE("element nav: a host that holds only the region is neutral, and the element sounds as the region did", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    REQUIRE(r.open("Demo"));
    writeClip((r.clipsDir("Demo") / "a.wav").string(), 2 * kBarFrames, 8192);
    writeClip((r.clipsDir("Demo") / "b.wav").string(), 2 * kBarFrames, 16384);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.bpm = 120.0f;
        s.tracks[0].inUse = s.tracks[1].inUse = true;
        s.tracks[0].volume = 0.5f;
        s.tracks[0].pan = 0.5f;                    // 0.25 left, 0.5 right of what the clip holds
        s.tracks[0].solo = true;
        s.tracks[1].muted = true;                  // silent in the song, and still in the element
        s.addClip(0, {"a.wav", 0, 2});
        s.addClip(1, {"b.wav", 0, 2});
    }
    r.engine->commit();
    REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, 10000));
    REQUIRE(elementNav().makeFromRegion(0, 2));
    const auto res = runRendering(r);
    INFO("error: " << res.error);
    REQUIRE(res.ok);
    SongClip block;
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        const AudioSong& s = r.engine->song();
        REQUIRE(s.tracks[0].clips.size() == 1);
        block = s.tracks[0].clips[0];
        CHECK(block.isElement());
        CHECK(s.tracks[0].volume == 1.0f);         // the bake has the source's volume and pan already
        CHECK(s.tracks[0].pan == 0.0f);
        CHECK(s.tracks[0].solo);                   // the host's solo is the song's: other tracks stay silenced
        CHECK_FALSE(s.tracks[0].muted);
        CHECK(s.tracks[1].clips.empty());
    }
    const auto pcm = readWavData(bakeOf(r, block.file).string());
    REQUIRE(pcm.size() == (size_t)2 * kBarFrames * 2);
    for (size_t i : {size_t(0), pcm.size() / 2 - 2, pcm.size() - 2}) {
        CHECK(std::abs(pcm[i] - 2048) <= 1);       // 8192 * 0.5 * (1 - 0.5): the left
        CHECK(std::abs(pcm[i + 1] - 4096) <= 1);   // the right: 8192 * 0.5
    }
}

TEST_CASE("element nav: a region with no host is refused and changes nothing", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    REQUIRE(r.open("Demo"));
    while (r.seq.sceneCount() < 2) REQUIRE(r.seq.addScene());
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.bpm = 120.0f;
        s.tracks[0].inUse = true;
        s.addClip(0, {"a.wav", 0, 4});             // reaches outside the region
        REQUIRE(s.addSeqTrack(1, SeqSource::Pitch, 0));
        REQUIRE(s.addSeqTrack(2, SeqSource::Kit, 1));
        s.tracks[3].inUse = true;
        s.addClip(3, {"b.wav", 8, 1});
    }
    r.engine->commit();
    CHECK_FALSE(elementNav().makeFromRegion(1, 3));
    CHECK(elementNav().lastError().find("needs a free track") != std::string::npos);
    CHECK_FALSE(elementNav().busy());
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    REQUIRE(r.engine->song().tracks[0].clips.size() == 1);
    CHECK(r.engine->song().tracks[0].clips[0].bars == 4);
}

TEST_CASE("element nav: a song opened while the element is made is not cleared, and the element goes", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    regionDemo(r);
    REQUIRE(elementNav().makeFromRegion(1, 3));
    REQUIRE(waitFor([&] { return r.engine->editState() != SongOpState::Busy; }, 10000));   // made, not yet laid
    REQUIRE(r.open("Other"));                        // behind the job's back
    const auto res = runRendering(r);
    INFO("error: " << res.error);
    CHECK_FALSE(res.ok);
    CHECK(res.error.find("another song") != std::string::npos);
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        REQUIRE(r.engine->song().tracks[0].clips.size() == 1);
        CHECK(r.engine->song().tracks[0].clips[0].bars == 4);
    }
    const fs::path library = fs::path(r.platform.root) / AudioSong::elementsDir();
    CHECK(waitFor([&] { return !fs::exists(library) || fs::is_empty(library); }));   // discarded, none left in the library
}

TEST_CASE("element nav: a song holding an element on the breadcrumb is refused before it is copied", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    {
        AudioSong s;
        s.dir = r.platform.root + "/Src";
        fs::create_directories(fs::path(s.dir) / AudioSong::clipsDir());
        s.bpm = 120.0f;
        s.tracks[0].inUse = true;
        SongClip c;
        c.element = kA;
        c.bars = 2;
        s.addClip(0, c);
        REQUIRE(s.save());
    }
    openDemo(r, kA);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    REQUIRE(elementNav().insertSong(2, 0, "Src"));
    const auto res = runRendering(r);
    CHECK_FALSE(res.ok);
    CHECK(res.error.find("hold itself") != std::string::npos);
    const fs::path library = fs::path(r.platform.root) / AudioSong::elementsDir();
    size_t folders = 0;
    for (const auto& e : fs::directory_iterator(library)) { (void)e; ++folders; }
    CHECK(folders == 1);                             // Intro: the copy was never made
    CHECK(openName(r) == kA);
}

// ── the song heard around an element: parent blocks, passes, a frozen parent ──

TEST_CASE("element nav: entering freezes the parent's live seq tracks before the element opens", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    openDemo(r, kA);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        REQUIRE(s.addSeqTrack(2, SeqSource::Kit, 0));
        SeqNote n;
        n.pad = 1; n.vel = 100;
        s.putSeqTake(2, 0, makeSeqPattern(1, {n}), s.samplesPerBar(kRate));
        s.armed = -1;
    }
    r.engine->commit();
    REQUIRE(r.engine->freezeState(2) != FreezeState::Frozen);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(pollTo(r, "freezing"));
    CHECK(openName(r) == "Demo");                        // still the parent: nothing opened yet
    const auto res = runRendering(r);
    INFO("error: " << res.error);
    REQUIRE(res.ok);
    CHECK(openName(r) == kA);
    REQUIRE(elementNav().leave());
    REQUIRE(runRendering(r).ok);
    CHECK(openName(r) == "Demo");
    CHECK(r.engine->freezeState(2) == FreezeState::Frozen);
}

TEST_CASE("element nav: a level keeps the parent's blocks, the focus marked", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    openDemo(r, kA);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    const auto& blocks = elementNav().path()[1].parentBlocks;
    REQUIRE(blocks.size() == 1);
    CHECK(blocks[0].track == 1);
    CHECK(blocks[0].bar == 0);
    CHECK(blocks[0].bars == 4);
    CHECK(blocks[0].element);
    CHECK_FALSE(blocks[0].seq);
    CHECK(blocks[0].focus);
    CHECK(elementNav().path()[0].parentBlocks.empty());
}

TEST_CASE("element nav: the focus has a pass for every round of its block, through every level", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kB, "Hat", 2, 120.0f);
    writeAudioElement(r, kA, "Groove", 0, 120.0f, {kB});
    {
        AudioSong a;
        REQUIRE(a.load(r.platform.root + "/" + kA) == SongLoad::Loaded);
        a.length = 2;
        REQUIRE(a.save());
    }
    openDemo(r, kA);                                     // a 4-bar block of a 2-bar element: two passes
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    const auto& inA = elementNav().path()[1];
    REQUIRE(inA.occurrences.size() == 2);
    CHECK(inA.chosen == 0);
    CHECK(inA.occurrences[0].parentBar == 0);
    CHECK(inA.occurrences[0].songFrame == 0);
    CHECK(inA.occurrences[1].parentBar == 2);
    CHECK(inA.occurrences[1].songFrame == 2 * kBarFrames);

    SECTION("a pass that is not the first, chosen, is where the level below counts from") {
        REQUIRE(elementNav().chooseOccurrence(1));
        CHECK(elementNav().path()[1].chosen == 1);
        REQUIRE(elementNav().enter(1, 0));
        REQUIRE(runNav().ok);
        const auto& inB = elementNav().path()[2];
        REQUIRE_FALSE(inB.occurrences.empty());
        CHECK(inB.occurrences[0].parentBar == 0);
        CHECK(inB.occurrences[0].songFrame == 2 * kBarFrames);
    }
    SECTION("a pass there is none of is refused") {
        CHECK_FALSE(elementNav().chooseOccurrence(2));
        CHECK_FALSE(elementNav().lastError().empty());
        CHECK(elementNav().path()[1].chosen == 0);
    }
}

TEST_CASE("element nav: no pass is chosen at the song", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    REQUIRE(r.open("Demo"));
    CHECK_FALSE(elementNav().chooseOccurrence(0));
}

namespace {

/// The block on track 1 of "Demo" starts @p from bars into its element.
void startBlockAt(Rig& r, uint16_t from) {
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().tracks[1].clips[0].from = from;
    }
    r.engine->commit();
    REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, 10000));
}

} // namespace

TEST_CASE("element nav: a pass of a block cut inside its element is placed by the element's frame 0", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kB, "Hat", 2, 120.0f);
    writeAudioElement(r, kA, "Groove", 0, 120.0f, {kB});
    {
        AudioSong a;
        REQUIRE(a.load(r.platform.root + "/" + kA) == SongLoad::Loaded);
        a.length = 2;
        REQUIRE(a.save());
    }
    openDemo(r, kA);
    startBlockAt(r, 1);                                  // 4 bars of a 2-bar element, from its bar 1
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    const auto& inA = elementNav().path()[1];
    REQUIRE(inA.occurrences.size() == 3);                // bar 1..2, bars 0..2, bar 0..1
    const int64_t bar = kBarFrames;
    CHECK(inA.occurrences[0].songFrame == -bar);
    CHECK(inA.occurrences[0].offset == kBarFrames);
    CHECK(inA.occurrences[0].frames == kBarFrames);
    CHECK(inA.occurrences[0].parentBar == 0);
    CHECK(inA.occurrences[1].songFrame == bar);
    CHECK(inA.occurrences[1].offset == 0);
    CHECK(inA.occurrences[1].frames == 2 * kBarFrames);
    CHECK(inA.occurrences[2].songFrame == 3 * bar);
    CHECK(inA.occurrences[2].frames == kBarFrames);

    SECTION("the level below counts from the first pass's frame 0 and sounds only what it plays") {
        REQUIRE(elementNav().enter(1, 0));               // B, 2 bars from A's bar 0; A's pass plays only A's bar 1
        REQUIRE(runNav().ok);
        const auto& inB = elementNav().path()[2];
        REQUIRE(inB.occurrences.size() == 1);
        CHECK(inB.occurrences[0].songFrame == -bar);
        CHECK(inB.occurrences[0].offset == kBarFrames);
        CHECK(inB.occurrences[0].frames == kBarFrames);
        CHECK(inB.occurrences[0].parentBar == 1);
    }
    SECTION("a whole pass of the level above leaves the level below whole") {
        REQUIRE(elementNav().chooseOccurrence(1));
        REQUIRE(elementNav().enter(1, 0));
        REQUIRE(runNav().ok);
        const auto& inB = elementNav().path()[2];
        REQUIRE(inB.occurrences.size() == 1);
        CHECK(inB.occurrences[0].songFrame == bar);
        CHECK(inB.occurrences[0].offset == 0);
        CHECK(inB.occurrences[0].frames == 2 * kBarFrames);
    }
}

TEST_CASE("element nav: the pass under the bar entered is the one chosen", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    openDemo(r, kA);
    REQUIRE(elementNav().enter(1, 3));                   // the second pass of the 4-bar block
    REQUIRE(runNav().ok);
    const auto& inA = elementNav().path()[1];
    REQUIRE(inA.occurrences.size() == 2);
    CHECK(inA.chosen == 1);
    CHECK(inA.occurrences[inA.chosen].parentBar == 2);
}

TEST_CASE("element nav: a parent block says whether it is baked or frozen", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    openDemo(r, kA);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        SongClip c;
        c.element = kA;
        c.bar = 8;
        s.addClip(0, c);                                 // laid, never baked
        s.tracks[0].inUse = true;
    }
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    bool baked = false, unbaked = false;
    for (const auto& b : elementNav().path()[1].parentBlocks) {
        if (b.track == 1) baked = b.ready;
        if (b.track == 0) unbaked = !b.ready;
    }
    CHECK(baked);
    CHECK(unbaked);
}

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void spit(const fs::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << bytes;
}

/// An edit request the worker has finished, as it ended.
SongOpState editEnd(Rig& r) {
    waitFor([&] { return r.engine->editState() != SongOpState::Busy; }, 10000);
    return r.engine->editState();
}

fs::path elementDir(Rig& r, const std::string& ref) { return fs::path(r.platform.root) / ref; }

/// kA with scenes of its own beside its song.json.
void writeElementWithScenes(Rig& r, const std::string& ref) {
    writeAudioElement(r, ref, "Intro", 2, 120.0f);
    spit(elementDir(r, ref) / "sequences.json", "{\"scenes\":[1,2,3]}");
}

/// A block of the song open, laid in the element open inside it: a recording's
/// worth of difference in what the element sounds.
void editOpenElement(Rig& r) {
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.tracks[2].inUse = true;
        s.addClip(2, {"a.wav", 0, 1});
    }
    r.engine->commit();
}

/// A project @p name on the card playing @p ref on @p track from bar 0, as a
/// song that placed it holds it: its bake @p bake, when given.
void writeSongUsing(Rig& r, const std::string& name, const std::string& ref, const std::string& bake = {}, int track = 1) {
    AudioSong s;
    s.dir = r.platform.root + "/" + name;
    fs::create_directories(fs::path(s.dir) / AudioSong::clipsDir());
    s.bpm = 120.0f;
    s.tracks[track].inUse = true;
    SongClip c;
    c.element = ref;
    c.bars = 2;
    c.file = bake;
    if (!bake.empty()) c.frames = 2 * kBarFrames;
    s.addClip(track, c);
    REQUIRE(s.save());
}

/// Polls a leave until it asks, or ends.
bool pollUntilAsking() {
    waitFor([&] { return elementNav().poll().done || elementNav().asking(); }, 20000);
    return elementNav().asking();
}

std::vector<ElementUse> usesNow(Rig& r, const std::string& ref) {
    const uint32_t gen = r.engine->listGeneration();
    REQUIRE(r.engine->requestUses());
    REQUIRE(waitFor([&] { return r.engine->listGeneration() != gen; }));
    return r.engine->usesOf(ref);
}

} // namespace

TEST_CASE("element uses: the songs and the elements that play an element are found", "[element][uses]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Hat", 2, 120.0f);
    writeAudioElement(r, kB, "Groove", 2, 120.0f, {kA});
    writeAudioElement(r, kC, "Lonely", 2, 120.0f);
    {
        AudioSong other;
        other.dir = r.platform.root + "/Other";
        fs::create_directories(fs::path(other.dir) / AudioSong::clipsDir());
        other.tracks[1].inUse = true;
        SongClip c;
        c.element = kA;
        c.bars = 2;
        other.addClip(1, c);
        c.bar = 4;
        other.addClip(1, c);                              // twice in one song: one use
        REQUIRE(other.save());
    }
    openDemo(r, kA);                                      // in memory only: the save is not due yet

    const auto uses = usesNow(r, kA);
    REQUIRE(uses.size() == 3);
    CHECK(uses[0].user == "Demo");
    CHECK(uses[0].song);
    CHECK(uses[1].user == "Other");
    CHECK(uses[1].song);
    CHECK(uses[2].user == kB);
    CHECK_FALSE(uses[2].song);
    CHECK(r.engine->usesOf(kC).empty());
    CHECK(r.engine->usesOf("../etc").empty());
}

TEST_CASE("element snapshot: song.json and sequences.json are kept beside the element, none overwritten", "[element][snapshot]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElementWithScenes(r, kA);
    REQUIRE(r.open("Demo"));
    const fs::path dir = elementDir(r, kA);
    const std::string song = slurp(dir / "song.json"), scenes = slurp(dir / "sequences.json");
    REQUIRE_FALSE(r.engine->snapshotPending(kA));

    REQUIRE(r.engine->requestSnapshot(kA));
    REQUIRE(editEnd(r) == SongOpState::Ready);
    CHECK(slurp(dir / "song.orig.json") == song);
    CHECK(slurp(dir / "sequences.orig.json") == scenes);
    CHECK(r.engine->snapshotPending(kA));

    spit(dir / "song.json", "changed");
    REQUIRE(r.engine->requestSnapshot(kA));
    REQUIRE(editEnd(r) == SongOpState::Ready);
    CHECK(slurp(dir / "song.orig.json") == song);        // the first one is the original
    CHECK_FALSE(fs::exists(dir / "song.orig.json.part"));
}

TEST_CASE("element snapshot: a ref or the open element that is no element's is refused", "[element][snapshot]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    REQUIRE(r.open("Demo"));
    CHECK_FALSE(r.engine->requestSnapshot("../Demo"));
    CHECK_FALSE(r.engine->requestSnapshot(".elements/../Demo"));
    CHECK_FALSE(r.engine->requestRestore("Demo"));
    CHECK_FALSE(r.engine->requestDropSnapshot(".elements/x"));
    REQUIRE(r.engine->requestOpenElement(kA));
    REQUIRE(waitFor([&] { return r.engine->openState() == SongOpState::Ready && openName(r) == kA; }));
    REQUIRE(r.engine->requestSnapshot(kA));               // the open one is not kept: it is not the card's
    CHECK(editEnd(r) == SongOpState::Failed);
    CHECK_FALSE(fs::exists(elementDir(r, kA) / "song.orig.json"));
}

TEST_CASE("element snapshot: restore puts the files back byte for byte and removes the snapshot", "[element][snapshot]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElementWithScenes(r, kA);
    REQUIRE(r.open("Demo"));
    const fs::path dir = elementDir(r, kA);
    const std::string song = slurp(dir / "song.json"), scenes = slurp(dir / "sequences.json");

    REQUIRE(r.engine->requestRestore(kA));
    CHECK(editEnd(r) == SongOpState::Failed);             // nothing was kept

    REQUIRE(r.engine->requestSnapshot(kA));
    REQUIRE(editEnd(r) == SongOpState::Ready);
    spit(dir / "song.json", "edited");
    spit(dir / "sequences.json", "edited too");
    REQUIRE(r.engine->requestRestore(kA));
    REQUIRE(editEnd(r) == SongOpState::Ready);
    CHECK(slurp(dir / "song.json") == song);
    CHECK(slurp(dir / "sequences.json") == scenes);
    CHECK_FALSE(fs::exists(dir / "song.orig.json"));
    CHECK_FALSE(fs::exists(dir / "sequences.orig.json"));
    CHECK_FALSE(r.engine->snapshotPending(kA));
}

TEST_CASE("element snapshot: scenes made after the snapshot go with the restore", "[element][snapshot]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);          // no scenes of its own
    REQUIRE(r.open("Demo"));
    const fs::path dir = elementDir(r, kA);
    REQUIRE(r.engine->requestSnapshot(kA));
    REQUIRE(editEnd(r) == SongOpState::Ready);
    CHECK_FALSE(fs::exists(dir / "sequences.orig.json"));
    spit(dir / "sequences.json", "made since");
    REQUIRE(r.engine->requestRestore(kA));
    REQUIRE(editEnd(r) == SongOpState::Ready);
    CHECK_FALSE(fs::exists(dir / "sequences.json"));
}

TEST_CASE("element snapshot: a snapshot dropped leaves the element, and none is no failure", "[element][snapshot]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElementWithScenes(r, kA);
    REQUIRE(r.open("Demo"));
    const fs::path dir = elementDir(r, kA);
    const std::string song = slurp(dir / "song.json");
    REQUIRE(r.engine->requestSnapshot(kA));
    REQUIRE(editEnd(r) == SongOpState::Ready);
    REQUIRE(r.engine->requestDropSnapshot(kA));
    REQUIRE(editEnd(r) == SongOpState::Ready);
    CHECK_FALSE(r.engine->snapshotPending(kA));
    CHECK_FALSE(fs::exists(dir / "song.orig.json"));
    CHECK_FALSE(fs::exists(dir / "sequences.orig.json"));
    CHECK(slurp(dir / "song.json") == song);
    REQUIRE(r.engine->requestDropSnapshot(kA));
    CHECK(editEnd(r) == SongOpState::Ready);
}

TEST_CASE("element snapshot: the list says which elements have one", "[element][snapshot]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    writeAudioElement(r, kB, "Other", 2, 120.0f);
    REQUIRE(r.open("Demo"));
    spit(elementDir(r, kB) / "song.orig.json", "{}");      // left by a power cut
    CHECK_FALSE(r.engine->snapshotPending(kB));            // not read yet
    REQUIRE(listElements(r));
    CHECK(r.engine->snapshotPending(kB));
    CHECK_FALSE(r.engine->snapshotPending(kA));
}

TEST_CASE("element nav: entering keeps the element as it is, and leaving it unedited takes that off again", "[element][nav][snapshot]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElementWithScenes(r, kA);
    openDemo(r, kA);
    const fs::path dir = elementDir(r, kA);
    const std::string song = slurp(dir / "song.json"), scenes = slurp(dir / "sequences.json");

    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    CHECK(slurp(dir / "song.orig.json") == song);
    CHECK(slurp(dir / "sequences.orig.json") == scenes);
    CHECK(r.engine->snapshotPending(kA));
    CHECK(elementNav().path()[1].hashAtEntry != 0);
    CHECK(elementNav().path()[1].snapshotTaken);

    REQUIRE(elementNav().leave());
    REQUIRE(runNav().ok);
    CHECK_FALSE(elementNav().changed());
    CHECK(elementNav().left() == kA);
    REQUIRE(waitFor([&] { return !r.engine->snapshotPending(kA); }, 5000));
    CHECK_FALSE(fs::exists(dir / "song.orig.json"));
    CHECK_FALSE(fs::exists(dir / "sequences.orig.json"));
}

TEST_CASE("element nav: an element edited inside and played elsewhere keeps its snapshot until it is decided", "[element][nav][snapshot]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElementWithScenes(r, kA);
    openDemo(r, kA);
    writeSongUsing(r, "Other", kA);
    const fs::path dir = elementDir(r, kA);
    const std::string song = slurp(dir / "song.json"), scenes = slurp(dir / "sequences.json");

    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    editOpenElement(r);
    REQUIRE(elementNav().leave());
    REQUIRE(pollUntilAsking());
    CHECK(r.engine->snapshotPending(kA));
    CHECK(slurp(dir / "song.orig.json") == song);
    CHECK(slurp(dir / "sequences.orig.json") == scenes);
    REQUIRE(elementNav().choose(ElementNav::Choice::Replace));
    const auto out = runNav();
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    CHECK(elementNav().changed());
    CHECK_FALSE(r.engine->snapshotPending(kA));
    CHECK_FALSE(fs::exists(dir / "song.orig.json"));
    CHECK_FALSE(fs::exists(dir / "sequences.orig.json"));
}

TEST_CASE("element nav: an element edited inside that nothing else plays keeps its edit, with nothing to ask", "[element][nav][snapshot]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    openDemo(r, kA);
    const fs::path dir = elementDir(r, kA);
    const std::string song = slurp(dir / "song.json");

    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    editOpenElement(r);
    REQUIRE(elementNav().leave());
    const auto out = runNav();
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    CHECK(elementNav().changed());
    CHECK(clipOf(r, 1).element == kA);
    CHECK_FALSE(r.engine->snapshotPending(kA));
    CHECK_FALSE(fs::exists(dir / "song.orig.json"));
    REQUIRE(waitFor([&] { return slurp(dir / "song.json") != song; }, 5000));
}

TEST_CASE("element nav: the uses are read after a leave and at the first poll", "[element][nav][uses]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    openDemo(r, kA);
    CHECK(r.engine->usesOf(kA).empty());
    elementNav().poll();                                   // Arrange's first tick
    REQUIRE(waitFor([&] { return !r.engine->usesOf(kA).empty(); }, 5000));
    CHECK(r.engine->usesOf(kA).front().user == "Demo");
}

TEST_CASE("element nav: an element nobody touched is not changed by the fit to the song's tempo", "[element][nav][snapshot]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElementWithScenes(r, kA);
    {
        AudioSong s;
        REQUIRE(s.load(r.platform.root + "/" + kA) == SongLoad::Loaded);
        s.bpm = 90.0f;                                    // made at another tempo than "Demo"
        REQUIRE(s.save());
    }
    openDemo(r, kA);
    const fs::path dir = elementDir(r, kA);

    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        CHECK(r.engine->song().tracks[0].stretch == StretchMode::KeepPitch);   // the enter's own fit
    }
    REQUIRE(elementNav().leave());
    REQUIRE(runNav().ok);
    CHECK_FALSE(elementNav().changed());
    REQUIRE(waitFor([&] { return !r.engine->snapshotPending(kA); }, 5000));
    CHECK_FALSE(fs::exists(dir / "song.orig.json"));
}

TEST_CASE("element nav: a tempo fit and an edit together are still an edit", "[element][nav][snapshot]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Slow", 2, 90.0f);
    openDemo(r, kA);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    editOpenElement(r);
    REQUIRE(elementNav().leave());
    REQUIRE(runNav().ok);
    CHECK(elementNav().changed());
    CHECK_FALSE(r.engine->snapshotPending(kA));            // nothing else plays it: nothing to decide
}

TEST_CASE("element nav: a recording's stretch the player changed inside is an edit", "[element][nav][snapshot]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Loop", 2, 120.0f);
    openDemo(r, kA);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        REQUIRE(s.tracks[0].stretch == StretchMode::Off);   // entered at its own tempo: nothing fitted
        s.tracks[0].stretch = StretchMode::Repitch;
    }
    r.engine->commit();
    REQUIRE(elementNav().leave());
    const auto res = runNav();
    INFO("error: " << res.error);
    REQUIRE(res.ok);
    CHECK(elementNav().changed());
}

TEST_CASE("element nav: a scene edited inside is an edit", "[element][nav][snapshot]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElementWithScenes(r, kA);
    openDemo(r, kA);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    spit(elementDir(r, kA) / "sequences.json", "{\"scenes\":[9]}");
    REQUIRE(elementNav().leave());
    REQUIRE(runNav().ok);
    CHECK(elementNav().changed());
}

TEST_CASE("element snapshot: a list takes off a snapshot that is the element byte for byte", "[element][snapshot]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElementWithScenes(r, kA);
    writeElementWithScenes(r, kB);
    REQUIRE(r.open("Demo"));
    for (const char* ref : {kA, kB}) {
        const fs::path dir = elementDir(r, ref);
        spit(dir / "song.orig.json", slurp(dir / "song.json"));
        spit(dir / "sequences.orig.json", slurp(dir / "sequences.json"));
    }
    spit(elementDir(r, kB) / "sequences.json", "scenes made after");
    REQUIRE(listElements(r));
    CHECK_FALSE(r.engine->snapshotPending(kA));
    CHECK_FALSE(fs::exists(elementDir(r, kA) / "song.orig.json"));
    CHECK_FALSE(fs::exists(elementDir(r, kA) / "sequences.orig.json"));
    CHECK(r.engine->snapshotPending(kB));                  // an edit to decide on
    CHECK(fs::exists(elementDir(r, kB) / "song.orig.json"));
}

TEST_CASE("element snapshot: the element being edited keeps its snapshot through a list", "[element][nav][snapshot]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    openDemo(r, kA);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    REQUIRE(listElements(r));                              // nothing saved yet: the card equals the snapshot
    CHECK(r.engine->snapshotPending(kA));
    CHECK(fs::exists(elementDir(r, kA) / "song.orig.json"));
}

TEST_CASE("element snapshot: scenes left by a power cut are not brought back by a restore", "[element][snapshot]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);          // no scenes of its own
    REQUIRE(r.open("Demo"));
    const fs::path dir = elementDir(r, kA);
    spit(dir / "sequences.orig.json", "old scenes of a snapshot long gone");
    REQUIRE(r.engine->requestSnapshot(kA));
    REQUIRE(editEnd(r) == SongOpState::Ready);
    CHECK_FALSE(fs::exists(dir / "sequences.orig.json"));
    spit(dir / "sequences.json", "made since");
    REQUIRE(r.engine->requestRestore(kA));
    REQUIRE(editEnd(r) == SongOpState::Ready);
    CHECK_FALSE(fs::exists(dir / "sequences.json"));
}

TEST_CASE("element snapshot: a restore is refused while a song plays", "[element][snapshot]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElementWithScenes(r, kA);
    openDemo(r, kA);
    REQUIRE(r.engine->requestSnapshot(kA));
    REQUIRE(editEnd(r) == SongOpState::Ready);
    spit(elementDir(r, kA) / "song.json", "edited");
    REQUIRE(r.engine->play(0));
    REQUIRE(waitFor([&] { return r.engine->playing(); }));
    CHECK_FALSE(r.engine->requestRestore(kA));
    CHECK(slurp(elementDir(r, kA) / "song.json") == "edited");
    r.engine->stop();
    REQUIRE(waitFor([&] { return !r.engine->playing(); }));
    REQUIRE(r.engine->requestRestore(kA));
    CHECK(editEnd(r) == SongOpState::Ready);
}

namespace {

/// The block on @p track of the song open plays an element, named by ref.
std::string elementOf(Rig& r, int track) { return clipOf(r, track).element; }

AudioSong loadElement(Rig& r, const std::string& ref) {
    AudioSong s;
    REQUIRE(s.load(elementDir(r, ref).string()) == SongLoad::Loaded);
    return s;
}

size_t elementFolders(Rig& r) { return song_list_dirs(r.platform.root + "/" + AudioSong::elementsDir()).size(); }

/// A scenes file at @p path, one hit in the first scene, saved at @p bpm with
/// scene @p active selected.
void writeScenesFile(const std::string& path, float bpm, uint8_t active) {
    MetronomeNode clock;
    clock.onPrepare(kBlock, kRate);
    clock.setBpm(bpm);
    PatternSequencer seq;
    seq.attach(&clock);
    seq.resetScenes();
    REQUIRE(seq.addEventTicks(0, 0, 3, 100));
    seq.selectScene(active);
    REQUIRE(SequenceStore::save(seq, path));
}

/// kB ("Hat", a recording) inside kA ("Groove", two bars) on its track 1, kA
/// in "Demo" on track 1.
void openNest(Rig& r) {
    writeAudioElement(r, kB, "Hat", 2, 120.0f);
    writeAudioElement(r, kA, "Groove", 0, 120.0f, {kB});
    AudioSong a = loadElement(r, kA);
    a.length = 2;
    REQUIRE(a.save());
    openDemo(r, kA);
}

/// @p ref (@p title, two bars) holding kB on its track 1, laid in the song open on @p track.
void layHolder(Rig& r, const std::string& ref, const std::string& title, int track) {
    writeAudioElement(r, ref, title, 0, 120.0f, {kB});
    AudioSong c = loadElement(r, ref);
    c.length = 2;
    REQUIRE(c.save());
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.tracks[track].inUse = true;
        SongClip clip;
        clip.element = ref;
        clip.bars = 2;
        s.addClip(track, clip);
    }
    r.engine->commit();
    REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, 10000));
}

/// Enter kA and kB under it, and edit kB.
void editHatInGroove(Rig& r) {
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    REQUIRE(elementNav().enter(1, 0));
    const auto in = runNav();
    INFO("error: " << in.error);
    REQUIRE(in.ok);
    REQUIRE(elementNav().depth() == 2);
    editOpenElement(r);
}

} // namespace

TEST_CASE("element copy: Replace keeps the edit for everyone, and another song bakes it as it opens", "[element][nav][copy]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElementWithScenes(r, kA);
    openDemo(r, kA);
    const std::string oldBake = clipOf(r, 1).file;
    REQUIRE_FALSE(oldBake.empty());
    writeSongUsing(r, "Other", kA, oldBake);

    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    editOpenElement(r);
    REQUIRE(elementNav().leave());
    REQUIRE(pollUntilAsking());
    CHECK(elementNav().question().title == "Intro");
    CHECK(elementNav().question().uses == 1);              // "Other"; "Demo" is where it was edited
    CHECK(elementNav().question().copies == std::vector<std::string>{"Intro 2"});
    REQUIRE(elementNav().choose(ElementNav::Choice::Replace));
    const auto out = runNav();
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    CHECK(elementOf(r, 1) == kA);
    CHECK(loadElement(r, kA).tracks[2].clips.size() == 1);
    CHECK_FALSE(fs::exists(elementDir(r, kA) / "song.orig.json"));
    CHECK(elementFolders(r) == 1);

    REQUIRE(r.open("Other"));
    REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, 10000));
    CHECK(elementOf(r, 1) == kA);
    CHECK(clipOf(r, 1).file != oldBake);
    CHECK(r.engine->elementClipState(1, 0) == ElementClipState::Fresh);
}

TEST_CASE("element copy: Copy gives the song a new element with the edit and puts the original back", "[element][nav][copy]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElementWithScenes(r, kA);
    const fs::path dir = elementDir(r, kA);
    /* What a copy is made again from stays behind. */
    for (const char* made : {"clips/.freeze", "clips/.stretch", ".peaks"}) {
        fs::create_directories(dir / made);
        spit(dir / made / "x", "derived");
    }
    openDemo(r, kA);
    const std::string oldBake = clipOf(r, 1).file;
    writeSongUsing(r, "Other", kA, oldBake);
    const std::string song = slurp(dir / "song.json"), scenes = slurp(dir / "sequences.json");
    const std::string other = slurp(fs::path(r.platform.root) / "Other" / "song.json");

    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    editOpenElement(r);
    spit(dir / "sequences.json", "{\"scenes\":[7]}");
    REQUIRE(elementNav().leave());
    REQUIRE(pollUntilAsking());
    REQUIRE(elementNav().choose(ElementNav::Choice::Copy));
    const auto out = runNav();
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    CHECK(elementNav().depth() == 0);

    const std::string copy = elementOf(r, 1);
    REQUIRE(is_element_ref(copy));
    CHECK(copy != kA);
    const AudioSong made = loadElement(r, copy);
    CHECK(made.title == "Intro 2");
    CHECK(made.tracks[2].clips.size() == 1);                // the edit
    CHECK(slurp(elementDir(r, copy) / "sequences.json") == "{\"scenes\":[7]}");
    for (const char* left : {"song.orig.json", "sequences.orig.json", "clips/.stretch", ".peaks"})
        CHECK_FALSE(fs::exists(elementDir(r, copy) / left));
    CHECK(slurp(elementDir(r, copy) / "clips/.freeze/x") == "derived");   // freezes hold in the copy
    REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, 10000));
    CHECK(r.engine->elementClipState(1, 0) == ElementClipState::Fresh);

    CHECK(slurp(dir / "song.json") == song);                // the original, byte for byte
    CHECK(slurp(dir / "sequences.json") == scenes);
    CHECK_FALSE(fs::exists(dir / "song.orig.json"));
    CHECK_FALSE(fs::exists(dir / "sequences.orig.json"));
    CHECK_FALSE(r.engine->snapshotPending(kA));
    CHECK(slurp(fs::path(r.platform.root) / "Other" / "song.json") == other);
    CHECK(elementFolders(r) == 2);
}

TEST_CASE("element copy: no answer calls the leave off and changes nothing", "[element][nav][copy]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElementWithScenes(r, kA);
    openDemo(r, kA);
    writeSongUsing(r, "Other", kA);
    const std::string song = slurp(elementDir(r, kA) / "song.json");

    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    editOpenElement(r);
    REQUIRE(elementNav().leave());
    REQUIRE(pollUntilAsking());
    REQUIRE(elementNav().choose(ElementNav::Choice::None));
    CHECK_FALSE(elementNav().choose(ElementNav::Choice::Copy));   // answered once
    const auto out = runNav();
    CHECK(out.done);
    CHECK_FALSE(out.ok);
    CHECK(out.calledOff);
    CHECK(out.job == ElementNav::Job::Leave);
    CHECK(elementNav().depth() == 1);
    CHECK(openName(r) == kA);
    CHECK(r.engine->snapshotPending(kA));
    CHECK(slurp(elementDir(r, kA) / "song.orig.json") == song);
    CHECK(elementFolders(r) == 1);
    REQUIRE(elementNav().leave());
    CHECK(pollUntilAsking());                               // asked again
    REQUIRE(elementNav().choose(ElementNav::Choice::Replace));
    CHECK(runNav().ok);
}

TEST_CASE("element copy: two levels down, a shared parent is copied too, each once, other songs untouched", "[element][nav][copy]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    openNest(r);
    writeSongUsing(r, "Other", kA);
    writeSongUsing(r, "Other2", kB);
    const std::string a = slurp(elementDir(r, kA) / "song.json"), b = slurp(elementDir(r, kB) / "song.json");
    const fs::path root(r.platform.root);
    const std::string other = slurp(root / "Other" / "song.json"), other2 = slurp(root / "Other2" / "song.json");

    editHatInGroove(r);
    REQUIRE(elementNav().leave());
    REQUIRE(pollUntilAsking());
    CHECK(elementNav().question().uses == 1);
    CHECK(elementNav().question().copies == std::vector<std::string>{"Hat 2", "Groove 2"});
    REQUIRE(elementNav().choose(ElementNav::Choice::Copy));
    auto out = runNav();
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    REQUIRE(elementNav().depth() == 1);
    CHECK(elementNav().path()[1].copyOnLeave);
    const std::string hat2 = elementOf(r, 1);
    CHECK(hat2 != kB);

    REQUIRE(elementNav().leave());
    out = runNav();                                         // no question: the copy goes on up
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    REQUIRE(elementNav().depth() == 0);
    const std::string groove2 = elementOf(r, 1);
    CHECK(groove2 != kA);
    const AudioSong g = loadElement(r, groove2);
    CHECK(g.title == "Groove 2");
    CHECK(g.tracks[1].clips.at(0).element == hat2);
    CHECK(loadElement(r, hat2).tracks[2].clips.size() == 1);
    CHECK(slurp(elementDir(r, kA) / "song.json") == a);
    CHECK(slurp(elementDir(r, kB) / "song.json") == b);
    CHECK(slurp(root / "Other" / "song.json") == other);
    CHECK(slurp(root / "Other2" / "song.json") == other2);
    CHECK(elementFolders(r) == 4);
}

TEST_CASE("element copy: the copied element under another element of the song takes the copy there too", "[element][nav][copy]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    openNest(r);
    layHolder(r, kC, "Verse", 2);
    writeSongUsing(r, "Other", kB);
    const std::string b = slurp(elementDir(r, kB) / "song.json");

    editHatInGroove(r);
    REQUIRE(elementNav().leaveTo(0));
    REQUIRE(pollUntilAsking());
    CHECK(elementNav().question().copies == std::vector<std::string>{"Hat 2"});   // Groove and Verse are this song's
    REQUIRE(elementNav().choose(ElementNav::Choice::Copy));
    const auto out = runNav();
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    REQUIRE(elementNav().depth() == 0);
    CHECK(elementOf(r, 1) == kA);                          // heard only here: re-pointed in place
    CHECK(elementOf(r, 2) == kC);
    const std::string hat2 = loadElement(r, kA).tracks[1].clips.at(0).element;
    CHECK(hat2 != kB);
    CHECK(loadElement(r, kC).tracks[1].clips.at(0).element == hat2);
    CHECK(slurp(elementDir(r, kB) / "song.json") == b);
    CHECK(elementFolders(r) == 4);
}

TEST_CASE("element copy: an element of the song played elsewhere too is copied to take the copy", "[element][nav][copy]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    openNest(r);
    layHolder(r, kC, "Verse", 2);
    writeSongUsing(r, "Other", kB);
    writeSongUsing(r, "Other3", kC);
    const std::string c = slurp(elementDir(r, kC) / "song.json");

    editHatInGroove(r);
    REQUIRE(elementNav().leave());
    REQUIRE(pollUntilAsking());
    CHECK(elementNav().question().copies == std::vector<std::string>{"Hat 2", "Verse 2"});
    REQUIRE(elementNav().choose(ElementNav::Choice::Copy));
    REQUIRE(runNav().ok);
    REQUIRE(elementNav().leave());
    const auto out = runNav();
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    REQUIRE(elementNav().depth() == 0);
    const std::string hat2 = loadElement(r, kA).tracks[1].clips.at(0).element;
    const std::string verse2 = elementOf(r, 2);
    CHECK(verse2 != kC);
    const AudioSong v = loadElement(r, verse2);
    CHECK(v.title == "Verse 2");
    CHECK(v.tracks[1].clips.at(0).element == hat2);
    CHECK(slurp(elementDir(r, kC) / "song.json") == c);   // Other3's Verse plays the original
    CHECK(elementFolders(r) == 5);
}

TEST_CASE("element delete: refused while anything plays it, done when nothing does", "[element][delete]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    writeAudioElement(r, kB, "Hat", 2, 120.0f);
    writeAudioElement(r, kC, "Verse", 2, 120.0f, {kB});
    writeSongUsing(r, "Other", kA);
    REQUIRE(r.open("Demo"));
    REQUIRE(listElements(r));
    CHECK_FALSE(r.engine->requestDeleteElement("../Demo"));

    REQUIRE(r.engine->requestDeleteElement(kA));
    REQUIRE(editEnd(r) == SongOpState::Failed);
    CHECK(r.engine->editReport() == std::vector<std::string>{"Used in 1 place.", "Other"});
    CHECK(fs::exists(elementDir(r, kA)));
    REQUIRE(r.engine->requestDeleteElement(kB));
    REQUIRE(editEnd(r) == SongOpState::Failed);
    CHECK(r.engine->editReport() == std::vector<std::string>{"Used in 1 place.", "Verse"});

    REQUIRE(r.engine->requestDeleteElement(kC));
    REQUIRE(editEnd(r) == SongOpState::Ready);
    CHECK_FALSE(fs::exists(elementDir(r, kC)));
    REQUIRE(r.engine->requestDeleteElement(kB));             // nothing plays it now
    REQUIRE(editEnd(r) == SongOpState::Ready);
    CHECK_FALSE(fs::exists(elementDir(r, kB)));
}

TEST_CASE("element draft: a snapshot left by a power cut is offered back on entering, and undone", "[element][nav][draft]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElementWithScenes(r, kA);
    openDemo(r, kA);
    const fs::path dir = elementDir(r, kA);
    const std::string song = slurp(dir / "song.json"), scenes = slurp(dir / "sequences.json");
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    editOpenElement(r);
    spit(dir / "sequences.json", "{\"scenes\":[7]}");
    /* The power goes: the board comes back in the song, the edit on the card. */
    elementNav().reset();
    REQUIRE(r.open("Demo"));
    REQUIRE(slurp(dir / "song.json") != song);
    REQUIRE(listElements(r));
    REQUIRE(r.engine->snapshotPending(kA));

    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    CHECK(elementNav().draftWaiting());
    CHECK(slurp(dir / "song.orig.json") == song);          // never overwritten by the enter
    CHECK_FALSE(elementNav().leave());
    REQUIRE(elementNav().resolveDraft(false));
    CHECK_FALSE(elementNav().resolveDraft(false));          // undoing it already
    const auto out = runNav();
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    CHECK(out.job == ElementNav::Job::Enter);
    CHECK(elementNav().depth() == 1);
    CHECK(openName(r) == kA);
    CHECK_FALSE(elementNav().draftWaiting());
    CHECK(slurp(dir / "song.json") == song);
    CHECK(slurp(dir / "sequences.json") == scenes);
    CHECK(elementNav().path()[1].snapshotTaken);
    REQUIRE(elementNav().leave());
    REQUIRE(runNav().ok);
    CHECK_FALSE(elementNav().changed());
    CHECK_FALSE(fs::exists(dir / "song.orig.json"));
}

TEST_CASE("element draft: an unfinished edit kept is left as an edit", "[element][nav][draft]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElementWithScenes(r, kA);
    openDemo(r, kA);
    const fs::path dir = elementDir(r, kA);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    editOpenElement(r);
    elementNav().reset();
    REQUIRE(r.open("Demo"));
    REQUIRE(listElements(r));

    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    REQUIRE(elementNav().draftWaiting());
    REQUIRE(elementNav().resolveDraft(true));
    CHECK_FALSE(elementNav().draftWaiting());
    REQUIRE(elementNav().leave());
    const auto out = runNav();
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    CHECK(elementNav().changed());
    CHECK(loadElement(r, kA).tracks[2].clips.size() == 1);
    CHECK_FALSE(fs::exists(dir / "song.orig.json"));
}

TEST_CASE("element nav: scenes edited inside reach the card before the leave looks at them", "[element][nav][snapshot]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    writeScenesFile((elementDir(r, kA) / "sequences.json").string(), 120.0f, 0);
    openDemo(r, kA);
    SongScenes store(r.seq, nullptr);
    const auto sweep = [&] { store.sweep(r.dir.string()); };
    sweep();
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    sweep();
    REQUIRE(store.currentFile() == (elementDir(r, kA) / "sequences.json").string());
    r.seq.setSceneBars(0, 2);
    REQUIRE_FALSE(r.engine->scenesSaved());

    REQUIRE(elementNav().leave());
    /* The store's next sweep comes a while after the leave began. */
    constexpr int kSweepAtPoll = 40;
    int polls = 0;
    ElementNav::Result out;
    REQUIRE(waitFor([&] {
        out = elementNav().poll();
        if (++polls == kSweepAtPoll) sweep();
        return out.done;
    }, 20000));
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    CHECK(elementNav().changed());
}

TEST_CASE("element nav: scenes saved again at another tempo, another scene selected, are no edit", "[element][nav][snapshot]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    const std::string file = (elementDir(r, kA) / "sequences.json").string();
    writeScenesFile(file, 90.0f, 0);
    openDemo(r, kA);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    const std::string before = slurp(file);
    writeScenesFile(file, 120.0f, 2);                      // the store, after the clock moved
    REQUIRE(slurp(file) != before);
    REQUIRE(elementNav().leave());
    REQUIRE(runNav().ok);
    CHECK_FALSE(elementNav().changed());
}

TEST_CASE("element nav: an element laid from the library and played elsewhere asks nothing", "[element][nav][copy]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    writeSongUsing(r, "Other", kA);
    REQUIRE(r.open("Demo"));
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().bpm = 120.0f;
    }
    r.engine->commit();
    REQUIRE(listElements(r));
    REQUIRE(elementNav().insertExisting(1, 0, kA));
    ElementNav::Result out;
    bool asked = false;
    REQUIRE(waitFor([&] {
        out = elementNav().poll();
        asked = asked || elementNav().asking();
        return out.done || asked;
    }, 20000));
    CHECK_FALSE(asked);
    INFO("error: " << out.error);
    CHECK(out.ok);
}

TEST_CASE("element copy: a copy that fails part way leaves nothing behind", "[element][copy]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    writeAudioElement(r, kB, "Hat", 2, 120.0f, {kA});
    REQUIRE(r.open("Demo"));
    const std::string a = slurp(elementDir(r, kA) / "song.json");
    const std::string b = loadElement(r, kB).tracks[1].clips.at(0).element;
    CHECK_FALSE(r.engine->requestCopyElements({{"../Demo", "X", false}}, {}, {}));

    /* kA has no snapshot to go back to: found only after the copy and the re-point. */
    REQUIRE(r.engine->requestCopyElements({{kA, "Intro 2", true}}, {kB}, {}));
    REQUIRE(editEnd(r) == SongOpState::Failed);
    CHECK_FALSE(r.engine->editReport().empty());
    CHECK(r.engine->elementsCopied().empty());
    CHECK(elementFolders(r) == 2);
    CHECK(loadElement(r, kB).tracks[1].clips.at(0).element == b);
    CHECK(slurp(elementDir(r, kA) / "song.json") == a);
}

TEST_CASE("element copy: scenes that never reach the card make no copy, and the edit stays unfinished", "[element][nav][copy]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElementWithScenes(r, kA);
    openDemo(r, kA);
    writeSongUsing(r, "Other", kA);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    editOpenElement(r);
    r.seq.setSceneBars(0, 2);                              // no store sweeps here: never written
    REQUIRE(elementNav().leave());
    REQUIRE(pollUntilAsking());
    REQUIRE(elementNav().choose(ElementNav::Choice::Copy));
    const auto out = runNav();
    CHECK(out.done);
    CHECK_FALSE(out.ok);
    CHECK(out.error.find("unfinished") != std::string::npos);
    CHECK(elementNav().depth() == 0);
    CHECK(elementOf(r, 1) == kA);
    CHECK(elementFolders(r) == 1);
    CHECK(r.engine->snapshotPending(kA));
    CHECK(fs::exists(elementDir(r, kA) / "song.orig.json"));
}

TEST_CASE("element copy: shared parent, holders here and elsewhere -- one question names every copy, each made once", "[element][nav][copy]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    const char* const kD = ".elements/E00000004";
    openNest(r);
    layHolder(r, kC, "Verse", 2);                          // heard only in Demo
    layHolder(r, kD, "Bridge", 3);                         // played by Other4 too
    writeSongUsing(r, "Other", kA);
    writeSongUsing(r, "Other2", kB);
    writeSongUsing(r, "Other4", kD);
    const std::string d = slurp(elementDir(r, kD) / "song.json");

    editHatInGroove(r);
    REQUIRE(elementNav().leaveTo(0));
    REQUIRE(pollUntilAsking());
    CHECK(elementNav().question().uses == 3);              // Other2, Verse, Bridge
    CHECK(elementNav().question().copies == std::vector<std::string>{"Hat 2", "Groove 2", "Bridge 2"});
    REQUIRE(elementNav().choose(ElementNav::Choice::Copy));
    const auto out = runNav();
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    REQUIRE(elementNav().depth() == 0);
    const std::string groove2 = elementOf(r, 1), bridge2 = elementOf(r, 3);
    CHECK(groove2 != kA);
    CHECK(elementOf(r, 2) == kC);
    CHECK(bridge2 != kD);
    const std::string hat2 = loadElement(r, groove2).tracks[1].clips.at(0).element;
    CHECK(hat2 != kB);
    CHECK(loadElement(r, kC).tracks[1].clips.at(0).element == hat2);
    CHECK(loadElement(r, bridge2).tracks[1].clips.at(0).element == hat2);
    CHECK(loadElement(r, bridge2).title == "Bridge 2");
    CHECK(slurp(elementDir(r, kD) / "song.json") == d);
    CHECK(elementFolders(r) == 7);                         // A, B, C, D and one copy each of B, A, D
}

TEST_CASE("element nav: scenes edited inside that never reach the card are an edit all the same", "[element][nav][copy]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElementWithScenes(r, kA);
    openDemo(r, kA);
    writeSongUsing(r, "Other", kA);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    r.seq.setSceneBars(0, 2);                              // no store sweeps here: never written
    REQUIRE(elementNav().leave());
    REQUIRE(pollUntilAsking());
    REQUIRE(elementNav().choose(ElementNav::Choice::Replace));
    CHECK(runNav().ok);
}

TEST_CASE("element copy: one original put back per request", "[element][copy]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    writeAudioElement(r, kB, "Hat", 2, 120.0f);
    REQUIRE(r.open("Demo"));
    CHECK_FALSE(r.engine->requestCopyElements({{kA, "Intro 2", true}, {kB, "Hat 2", true}}, {}, {}));
    CHECK(elementFolders(r) == 2);
}

TEST_CASE("element snapshot: a restore the card fails part way leaves the element as it was", "[element][snapshot]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElementWithScenes(r, kA);
    REQUIRE(r.open("Demo"));
    const fs::path dir = elementDir(r, kA);
    REQUIRE(r.engine->requestSnapshot(kA));
    REQUIRE(editEnd(r) == SongOpState::Ready);
    spit(dir / "song.json", "edited");
    spit(dir / "sequences.json", "edited too");
    /* The song's copy cannot be written, wherever it is put. */
    fs::create_directories(dir / "song.json.part");
    fs::create_directories(dir / "song.json.restored.part");
    REQUIRE(r.engine->requestRestore(kA));
    CHECK(editEnd(r) == SongOpState::Failed);
    CHECK(slurp(dir / "song.json") == "edited");
    CHECK(slurp(dir / "sequences.json") == "edited too");
    CHECK(fs::exists(dir / "song.orig.json"));
    CHECK(fs::exists(dir / "sequences.orig.json"));
}

TEST_CASE("element delete: refused while a song plays", "[element][delete]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    writeAudioElement(r, kB, "Hat", 2, 120.0f);
    openDemo(r, kA);
    REQUIRE(r.engine->play(0));
    REQUIRE(waitFor([&] { return r.engine->playing(); }));
    CHECK_FALSE(r.engine->requestDeleteElement(kB));
    r.engine->stop();
    REQUIRE(waitFor([&] { return !r.engine->playing(); }));
    CHECK(fs::exists(elementDir(r, kB)));
}

// ── a kit changed under the name a freeze plays it by ─────────────────────

namespace {

const char* const kSavedKit = "/crosspad/kits/Saved/kit.json";

/// The kits on the rig's card: kitFsPath() reads them under its folder.
struct KitMount {
    explicit KitMount(Rig& r) { setKitPathFsPrefix(r.dir.string()); }
    ~KitMount() { setKitPathFsPrefix(""); }
};

/// kSavedKit on the card holding @p text, written in place as a save of the kit would.
void writeSavedKit(const std::string& text) {
    const fs::path path = fs::path(kitFsPath(kSavedKit));
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

/// kA in "Demo" given a seq track playing kSavedKit, left: frozen and baked, its snapshot gone.
void leaveWithKitTrack(Rig& r) {
    writeAudioElement(r, kA, "Beat", 2, 120.0f);
    openDemo(r, kA);
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.kit = kSavedKit;
        REQUIRE(s.addSeqTrack(1, SeqSource::Kit, 0));
        SeqNote n;
        n.pad = 1; n.vel = 100;
        s.putSeqTake(1, 0, makeSeqPattern(1, {n}), s.samplesPerBar(kRate));
        s.armed = -1;
    }
    r.engine->commit();
    REQUIRE(elementNav().leave());
    const auto res = runRendering(r);
    INFO("error: " << res.error);
    REQUIRE(res.ok);
    REQUIRE(openName(r) == "Demo");
    REQUIRE(waitFor([&] { return !r.engine->snapshotPending(kA) && !r.engine->backgroundBusy(); }, 10000));
    REQUIRE(r.engine->elementClipState(1, 0) == ElementClipState::Fresh);
}

} // namespace

TEST_CASE("element nav: a kit changed on the card since an element froze leaves its block stale, never baked from the old freeze", "[element][nav][kit]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    KitMount mount(r);
    writeSavedKit("{\"name\":\"Saved\",\"pads\":1}");
    leaveWithKitTrack(r);
    const std::string frozenBake = clipOf(r, 1).file;

    /* Saved in another boot: nothing in memory heard of it. */
    writeSavedKit("{\"name\":\"Saved\",\"pads\":2}");
    REQUIRE(r.open("Demo"));
    REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, 10000));
    CHECK(clipOf(r, 1).file == frozenBake);
    CHECK(r.engine->elementClipState(1, 0) == ElementClipState::Stale);
    CHECK(r.engine->elementClipWhy(1, 0).find("kit changed") != std::string::npos);
}

TEST_CASE("element nav: a kit saved right after the element opened leaves it stale, and the next leave freezes it again", "[element][nav][kit]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    KitMount mount(r);
    writeSavedKit("{\"name\":\"Saved\",\"pads\":1}");
    leaveWithKitTrack(r);
    const std::string first = clipOf(r, 1).file;

    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    /* Before anything that watches the kits has looked: the track still reads as frozen. */
    writeSavedKit("{\"name\":\"Saved\",\"pads\":2}");
    REQUIRE(r.engine->freezeState(1) == FreezeState::Frozen);
    /* Nothing of it needs the engine: the song above opens at once, and the
     * card says the freeze is of the old kit. */
    REQUIRE(elementNav().leave());
    REQUIRE(runRendering(r).ok);
    REQUIRE(openName(r) == "Demo");
    REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, 10000));
    CHECK(clipOf(r, 1).file == first);
    CHECK(r.engine->elementClipState(1, 0) == ElementClipState::Stale);
    CHECK(r.engine->elementClipWhy(1, 0).find("kit changed") != std::string::npos);

    /* Opened again, the element knows: the leave freezes the track anew. */
    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    CHECK(r.engine->freezeState(1) == FreezeState::Stale);
    REQUIRE(elementNav().leave());
    const auto res = runRendering(r);
    INFO("error: " << res.error);
    REQUIRE(res.ok);
    CHECK(loadElement(r, kA).tracks[1].freezeSalt > 0);       // rendered anew, not found by its old name
    REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, 10000));
    CHECK(clipOf(r, 1).file != first);
    CHECK(r.engine->elementClipState(1, 0) == ElementClipState::Fresh);
}

TEST_CASE("element nav: a kit saved right after the element opened is frozen again by a leave that bakes on the engine", "[element][nav][kit]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    KitMount mount(r);
    writeSavedKit("{\"name\":\"Saved\",\"pads\":1}");
    leaveWithKitTrack(r);
    const std::string first = clipOf(r, 1).file;

    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    /* A new seq track to freeze: this leave bakes on the engine. */
    while (r.seq.sceneCount() < 2) REQUIRE(r.seq.addScene());
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        REQUIRE(s.addSeqTrack(2, SeqSource::Pitch, 1));
        SeqNote n;
        n.pad = 2; n.vel = 100;
        s.putSeqTake(2, 0, makeSeqPattern(1, {n}), s.samplesPerBar(kRate));
        s.armed = -1;
    }
    r.engine->commit();
    writeSavedKit("{\"name\":\"Saved\",\"pads\":2}");
    REQUIRE(r.engine->freezeState(1) == FreezeState::Frozen);
    REQUIRE(elementNav().leave());
    const auto res = runRendering(r);
    INFO("error: " << res.error);
    REQUIRE(res.ok);
    CHECK(openName(r) == "Demo");
    CHECK(loadElement(r, kA).tracks[1].freezeSalt > 0);
    REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, 10000));
    CHECK(clipOf(r, 1).file != first);
    CHECK(clipOf(r, 1).file == r.engine->bakeResult().file);
    CHECK(r.engine->elementClipState(1, 0) == ElementClipState::Fresh);
}

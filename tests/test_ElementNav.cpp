// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>

#include "song_engine_rig.hpp"

#include "crosspad/song/ElementNav.hpp"
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
    for (const auto& e : fs::directory_iterator(fs::path(r.platform.root) / AudioSong::bakeDir()))
        bakes.push_back(e.path().filename().string());
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

} // namespace

TEST_CASE("element nav: enter opens the element, leave bakes it and links the parent", "[element][nav]") {
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
    const SongClip c = clipOf(r, 1);
    CHECK_FALSE(c.file.empty());
    CHECK(c.file == r.engine->bakeResult().file);
    CHECK(c.frames == 2 * kBarFrames);
    CHECK(fs::exists(bakeOf(r, c.file)));
    CHECK(segmentsOf(*r.engine, 1) > 0);
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
        CHECK(clipOf(r, 1).file == r.engine->bakeResult().file);
        REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, 10000));
        CHECK(clipOf(r, 1).file == r.engine->bakeResult().file);
        CHECK(r.engine->elementClipState(1, 0) == ElementClipState::Fresh);
    }
    /* B's bake and A's: the same two whoever made them. */
    size_t bakes = 0;
    for (const auto& e : fs::directory_iterator(fs::path(r.platform.root) / AudioSong::bakeDir())) {
        (void)e;
        ++bakes;
    }
    CHECK(bakes == 2);
}

TEST_CASE("element nav: leaving unchanged finds the bake in the cache", "[element][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeAudioElement(r, kA, "Intro", 2, 120.0f);
    openDemo(r, kA);
    for (int round = 0; round < 2; ++round) {
        REQUIRE(elementNav().enter(1, 0));
        REQUIRE(runNav().ok);
        REQUIRE(elementNav().leave());
        REQUIRE(runNav().ok);
        /* The first time too: the background baked it when the block was laid, by the same key. */
        CHECK(r.engine->bakeResult().cached);
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
        fileInA = clipOf(r, 1).file;
        REQUIRE(elementNav().leave());
        REQUIRE(runNav().ok);
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

TEST_CASE("element nav: leave while playing stops, bakes and opens the parent", "[element][nav]") {
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
    CHECK(clipOf(r, 1).file == r.engine->bakeResult().file);
}

TEST_CASE("element nav: a bake that fails keeps you in the element", "[element][nav]") {
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
    REQUIRE(elementNav().leave());
    const auto out = runNav();
    CHECK(out.done);
    CHECK_FALSE(out.ok);
    CHECK_FALSE(out.error.empty());
    CHECK(elementNav().lastError() == out.error);
    CHECK_FALSE(elementNav().busy());
    CHECK(elementNav().depth() == 1);
    CHECK(openName(r) == kA);
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
    CHECK_FALSE(r.engine->bakeResult().cached);
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
    CHECK(r.engine->bakeResult().frames == 2 * kBarFrames);  // two bars at the song's 120, not at 100
    CHECK(clipOf(r, 1).frames == 2 * kBarFrames);
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
    CHECK(r.engine->snapshotKey() != 0);

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

TEST_CASE("element nav: an element edited inside is reported changed and keeps its snapshot, which restores it", "[element][nav][snapshot]") {
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
    REQUIRE(elementNav().leave());
    const auto out = runNav();
    INFO("error: " << out.error);
    REQUIRE(out.ok);
    CHECK(elementNav().changed());
    CHECK(r.engine->snapshotPending(kA));
    CHECK(slurp(dir / "song.orig.json") == song);
    CHECK(slurp(dir / "sequences.orig.json") == scenes);
    REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, 10000));
    REQUIRE(waitFor([&] { return slurp(dir / "song.json") != song; }, 5000));   // the edit is on the card

    REQUIRE(r.engine->requestRestore(kA));
    REQUIRE(editEnd(r) == SongOpState::Ready);
    CHECK(slurp(dir / "song.json") == song);
    CHECK(slurp(dir / "sequences.json") == scenes);
    CHECK_FALSE(r.engine->snapshotPending(kA));
}

TEST_CASE("element nav: entering again never overwrites the snapshot of an edit still undecided", "[element][nav][snapshot]") {
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
    REQUIRE(runNav().ok);
    REQUIRE(elementNav().changed());
    REQUIRE(waitFor([&] { return slurp(dir / "song.json") != song; }, 5000));

    REQUIRE(elementNav().enter(1, 0));
    REQUIRE(runNav().ok);
    CHECK_FALSE(elementNav().path()[1].snapshotTaken);     // it was there already
    REQUIRE(elementNav().leave());
    REQUIRE(runNav().ok);
    CHECK_FALSE(elementNav().changed());
    CHECK(slurp(dir / "song.orig.json") == song);          // still the original, not the first edit
    CHECK(r.engine->snapshotPending(kA));
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

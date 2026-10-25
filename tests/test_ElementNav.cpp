// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

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
        CHECK(r.engine->bakeResult().cached == (round == 1));
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
        CHECK(clipOf(r, 1).file.empty());
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
    CHECK(clipOf(r, 1).file.empty());
    CHECK_FALSE(fs::exists(fs::path(r.platform.root) / AudioSong::bakeDir()));
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

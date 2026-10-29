// SPDX-License-Identifier: GPL-3.0-or-later
//
// The context stem: inside an element, the song around it -- every occurrence
// of the element silent -- baked over one pass and played under it.

#include "song_engine_rig.hpp"

#include "crosspad/song/ElementNav.hpp"

#include <set>

namespace {

constexpr uint32_t kBar = 2 * kRate;                        // a bar at 120 BPM
constexpr int kWaitMs = 10000;
const char* const kE = ".elements/E00000001";
const char* const kF = ".elements/E00000002";
const char* const kG = ".elements/E00000003";

struct NavGuard {
    NavGuard() { elementNav().reset(); }
    ~NavGuard() { elementNav().reset(); }
};

/// An element block inside an element written by writeElement().
struct Use {
    std::string ref;
    uint32_t    bar = 0;
    uint16_t    bars = 1;
};

/// An element on the card at 120 BPM: track 0 @p bars bars of the constant
/// @p value, and each of @p uses on the tracks after it.
void writeElement(Rig& r, const std::string& ref, uint16_t bars, int16_t value, const std::vector<Use>& uses = {}) {
    AudioSong s;
    s.dir = r.platform.root + "/" + ref;
    fs::create_directories(fs::path(s.dir) / AudioSong::clipsDir());
    s.bpm = 120.0f;
    writeClip(s.clipPath("own.wav"), bars * kBar, value);
    s.tracks[0].inUse = true;
    s.addClip(0, {"own.wav", 0, bars});
    for (size_t i = 0; i < uses.size(); ++i) {
        SongClip c;
        c.element = uses[i].ref;
        c.bar = uses[i].bar;
        c.bars = uses[i].bars;
        s.tracks[1 + i].inUse = true;
        s.addClip((int)(1 + i), c);
    }
    REQUIRE(s.save());
}

/// "Demo" open at 120 BPM: on track 0 the clips @p bed (file, bar, bars, value),
/// and element blocks @p blocks on the tracks they name; the background through.
struct Bed {
    std::string file;
    uint32_t    bar;
    uint16_t    bars;
    int16_t     value;
};
struct Block {
    int         track;
    std::string ref;
    uint32_t    bar;
    uint16_t    bars;
};
void openDemo(Rig& r, const std::vector<Bed>& bed, const std::vector<Block>& blocks) {
    REQUIRE(r.open("Demo"));
    for (const Bed& b : bed) writeClip((r.clipsDir("Demo") / b.file).string(), b.bars * kBar, b.value);
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        s.bpm = 120.0f;
        s.tracks[0].inUse = true;
        for (const Bed& b : bed) s.addClip(0, {b.file, b.bar, b.bars});
        for (const Block& b : blocks) {
            SongClip c;
            c.element = b.ref;
            c.bar = b.bar;
            c.bars = b.bars;
            s.tracks[b.track].inUse = true;
            s.addClip(b.track, c);
        }
    }
    r.engine->commit();
    REQUIRE(waitFor([&] { return !r.engine->backgroundBusy(); }, kWaitMs));
}

ElementNav::Result runNav() {
    ElementNav::Result res;
    waitFor([&] { res = elementNav().poll(); return res.done; }, 20000);
    return res;
}

void enter(int track, uint32_t bar) {
    REQUIRE(elementNav().enter(track, bar));
    const auto res = runNav();
    INFO("error: " << res.error);
    REQUIRE(res.ok);
}

fs::path bakeDir(Rig& r) { return fs::path(r.platform.root) / AudioSong::bakeDir(); }

/// The context stems in the cache.
std::set<std::string> stems(Rig& r) {
    std::set<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(bakeDir(r), ec)) {
        const std::string n = e.path().filename().string();
        if (n[0] == 'C') out.insert(n);
    }
    return out;
}

bool contextIs(Rig& r, ContextState s) {
    return waitFor([&] { return r.engine->contextState() == s; }, kWaitMs);
}

/// The stem @p file is @p frames frames of @p value (within rounding).
void stemIs(Rig& r, const std::string& file, uint32_t frames, int16_t value) {
    const auto pcm = readWavData((bakeDir(r) / file).string());
    REQUIRE(pcm.size() == (size_t)frames * 2);
    for (size_t i : {size_t(0), pcm.size() / 2, pcm.size() - 1}) CHECK(std::abs(pcm[i] - value) <= 1);
}

/// Playing from the element's bar 0, renders until a whole block is @p level.
bool hears(Rig& r, float level) {
    std::vector<float> out(kBlock * 2), bus(kBlock * 2, 0.0f);
    return waitFor([&] {
        r.block(out, bus);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        return std::all_of(out.begin(), out.end(), [&](float v) { return std::fabs(v - level) < 0.002f; });
    }, kWaitMs);
}

int16_t q(float level) { return (int16_t)(level * 32768.0f); }

SongClip clipOf(Rig& r, int track) {
    std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
    return r.engine->song().tracks[track].clips.at(0);
}

/// The stem made since @p before, once the engine has one again in @p state.
std::string newStem(Rig& r, const std::set<std::string>& before, ContextState state) {
    std::string made;
    waitFor([&] {
        if (r.engine->contextState() != state) return false;
        for (const auto& f : stems(r))
            if (!before.count(f)) made = f;
        return !made.empty();
    }, kWaitMs);
    return made;
}

} // namespace

TEST_CASE("context: the song around the element plays under it, and SOLO leaves it out", "[song][element][context]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElement(r, kE, 4, q(0.5f));
    openDemo(r, {{"bed.wav", 0, 8, q(0.25f)}}, {{1, kE, 4, 4}});
    CHECK(r.engine->contextState() == ContextState::None);
    enter(1, 4);
    REQUIRE(contextIs(r, ContextState::Ready));
    CHECK(r.engine->contextWhy().empty());
    const auto files = stems(r);
    REQUIRE(files.size() == 1);
    stemIs(r, *files.begin(), 4 * kBar, q(0.25f));          // the song's bars 4..8, the element left out

    REQUIRE(r.engine->play(0));
    CHECK(hears(r, 0.75f));
    r.engine->contextSolo(true);
    CHECK(hears(r, 0.5f));
    r.engine->contextSolo(false);
    CHECK(hears(r, 0.75f));
    r.engine->stop();
    REQUIRE(waitFor([&] { return !r.engine->playing(); }));
}

TEST_CASE("context: the same pass asked for again is found in the cache", "[song][element][context]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElement(r, kE, 4, q(0.5f));
    openDemo(r, {{"bed.wav", 0, 8, q(0.25f)}}, {{1, kE, 4, 4}});
    enter(1, 4);
    REQUIRE(contextIs(r, ContextState::Ready));
    const auto first = stems(r);
    REQUIRE(first.size() == 1);
    const auto written = fs::last_write_time(bakeDir(r) / *first.begin());

    REQUIRE(elementNav().leave());
    REQUIRE(runNav().ok);
    CHECK(r.engine->contextState() == ContextState::None);   // left: no element to play around
    enter(1, 4);
    REQUIRE(contextIs(r, ContextState::Ready));
    CHECK(stems(r) == first);
    CHECK((fs::last_write_time(bakeDir(r) / *first.begin()) == written));
}

TEST_CASE("context: another pass of the element chosen bakes its own window", "[song][element][context]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElement(r, kE, 4, q(0.5f));
    openDemo(r, {{"low.wav", 0, 4, q(0.125f)}, {"high.wav", 4, 4, q(0.375f)}}, {{1, kE, 0, 4}, {1, kE, 4, 4}});
    enter(1, 4);
    REQUIRE(elementNav().path()[1].occurrences.size() == 2);
    REQUIRE(contextIs(r, ContextState::Ready));
    const auto first = stems(r);
    REQUIRE(first.size() == 1);
    stemIs(r, *first.begin(), 4 * kBar, q(0.375f));         // from the entered block's song frame

    REQUIRE(elementNav().chooseOccurrence(1));
    REQUIRE(waitFor([&] { return r.engine->contextState() == ContextState::Ready && stems(r).size() == 2; }, kWaitMs));
    std::string second;
    for (const auto& f : stems(r))
        if (!first.count(f)) second = f;
    stemIs(r, second, 4 * kBar, q(0.125f));
    REQUIRE(r.engine->play(0));
    CHECK(hears(r, 0.625f));
    r.engine->stop();
    REQUIRE(waitFor([&] { return !r.engine->playing(); }));
}

TEST_CASE("context: two levels down, the song and the element above play around it", "[song][element][context]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElement(r, kF, 2, q(0.5f));
    writeElement(r, kE, 4, q(0.0625f), {{kF, 2, 2}});
    openDemo(r, {{"bed.wav", 0, 8, q(0.25f)}}, {{1, kE, 4, 4}});
    enter(1, 4);
    enter(1, 2);                                             // F, at the song's bars 6..8
    REQUIRE(contextIs(r, ContextState::Ready));
    std::string stem;
    for (const auto& f : stems(r)) {
        if (readWavData((bakeDir(r) / f).string()).size() == (size_t)2 * kBar * 2) stem = f;
    }
    REQUIRE_FALSE(stem.empty());
    stemIs(r, stem, 2 * kBar, q(0.3125f));
    REQUIRE(r.engine->play(0));
    CHECK(hears(r, 0.8125f));
    r.engine->stop();
    REQUIRE(waitFor([&] { return !r.engine->playing(); }));
}

TEST_CASE("context: the element inside another element of the song is silent there too", "[song][element][context]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElement(r, kE, 4, q(0.5f));
    writeElement(r, kF, 4, q(0.0625f), {{kE, 0, 4}});
    openDemo(r, {{"bed.wav", 0, 8, q(0.25f)}}, {{1, kE, 4, 4}, {2, kF, 4, 4}});
    enter(1, 4);
    REQUIRE(contextIs(r, ContextState::Ready));
    const auto files = stems(r);
    REQUIRE(files.size() == 1);
    stemIs(r, *files.begin(), 4 * kBar, q(0.3125f));        // the bed and F's own track, not F's E
}

TEST_CASE("context: a seq track of the song never frozen leaves it incomplete, naming it", "[song][element][context]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    writeElement(r, kE, 4, q(0.5f));
    openDemo(r, {{"bed.wav", 0, 8, q(0.25f)}}, {{1, kE, 4, 4}});
    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        AudioSong& s = r.engine->song();
        REQUIRE(s.addSeqTrack(2, SeqSource::Kit, 0));
        SeqNote n;
        n.pad = 1;
        n.vel = 100;
        s.putSeqTake(2, 0, makeSeqPattern(1, {n}), s.samplesPerBar(kRate));
        s.armed = -1;
    }
    r.engine->commit();
    REQUIRE(r.engine->requestOpenElement(kE));
    REQUIRE(waitFor([&] {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        return r.engine->openState() == SongOpState::Ready && r.engine->song().name == kE;
    }));

    ContextRequest req;
    req.chain = {"Demo", kG};
    req.songFrame = 4 * kBar;
    req.frames = 4 * kBar;
    CHECK_FALSE(r.engine->requestContext(req));             // not the element open
    req.chain = {"Demo", kE};
    REQUIRE(r.engine->requestContext(req));
    REQUIRE(contextIs(r, ContextState::Incomplete));
    CHECK(r.engine->contextWhy().find("Track 3 is not frozen") != std::string::npos);
    const auto files = stems(r);
    REQUIRE(files.size() == 1);                              // what could be baked plays
    stemIs(r, *files.begin(), 4 * kBar, q(0.25f));

    REQUIRE(r.engine->requestContext(ContextRequest{}));
    CHECK(r.engine->contextState() == ContextState::None);
    CHECK(r.engine->contextWhy().empty());
}

TEST_CASE("context: an element not heard in the pass above has no pass and no context", "[song][element][context][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElement(r, kG, 2, q(0.5f));
    writeElement(r, kF, 2, q(0.25f), {{kG, 0, 2}});
    writeElement(r, kE, 4, q(0.125f), {{kF, 2, 2}});
    openDemo(r, {}, {{1, kE, 0, 1}});                        // only E's bar 0 is heard
    enter(1, 0);
    REQUIRE(elementNav().path()[1].occurrences.size() == 1);
    CHECK(contextIs(r, ContextState::Ready));               // nothing else in the song: nothing to bake
    CHECK(stems(r).empty());

    enter(1, 2);                                             // F, at E's bars 2..4: never heard
    CHECK(elementNav().path()[2].occurrences.empty());
    CHECK(r.engine->contextState() == ContextState::None);
    CHECK_FALSE(r.engine->contextWhy().empty());
    CHECK_FALSE(elementNav().chooseOccurrence(0));

    enter(1, 0);                                             // G, inside F: no more heard than F
    CHECK(elementNav().path()[3].occurrences.empty());
    CHECK(elementNav().path()[3].chosen == 0);
    CHECK(r.engine->contextState() == ContextState::None);
    CHECK_FALSE(r.engine->contextWhy().empty());
}

TEST_CASE("context: a stem made without an element's bake is made again once the bake is there", "[song][element][context]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElement(r, kE, 4, q(0.5f));
    writeElement(r, kF, 4, q(0.0625f));
    writeElement(r, kG, 4, q(0.125f));
    openDemo(r, {{"bed.wav", 0, 8, q(0.25f)}}, {{1, kE, 4, 4}, {2, kF, 4, 4}, {3, kG, 4, 4}});
    const std::string fBake = clipOf(r, 2).file, gBake = clipOf(r, 3).file;
    REQUIRE_FALSE(fBake.empty());
    REQUIRE_FALSE(gBake.empty());
    enter(1, 4);
    REQUIRE(contextIs(r, ContextState::Ready));
    auto before = stems(r);
    REQUIRE(before.size() == 1);
    stemIs(r, *before.begin(), 4 * kBar, q(0.4375f));

    /* Both bakes leave the card: the stem is made without them. */
    const fs::path kept = r.dir / "kept.wav";
    fs::copy_file(bakeDir(r) / fBake, kept);
    fs::remove(bakeDir(r) / fBake);
    fs::remove(bakeDir(r) / gBake);
    REQUIRE(elementNav().chooseOccurrence(0));
    std::string without = newStem(r, before, ContextState::Incomplete);
    REQUIRE_FALSE(without.empty());
    stemIs(r, without, 4 * kBar, q(0.25f));

    /* F's comes back: the stem without it is not taken from the cache. */
    before = stems(r);
    fs::copy_file(kept, bakeDir(r) / fBake);
    REQUIRE(elementNav().chooseOccurrence(0));
    const std::string withF = newStem(r, before, ContextState::Incomplete);
    REQUIRE_FALSE(withF.empty());
    stemIs(r, withF, 4 * kBar, q(0.3125f));
    CHECK(r.engine->contextWhy().find(kG) != std::string::npos);
}

TEST_CASE("context: an element whose bake is not made yet plays its older one in the stem", "[song][element][context]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElement(r, kE, 4, q(0.5f));
    writeElement(r, kF, 4, q(0.0625f));
    openDemo(r, {{"bed.wav", 0, 8, q(0.25f)}}, {{1, kE, 4, 4}, {2, kF, 4, 4}});
    enter(1, 4);
    REQUIRE(contextIs(r, ContextState::Ready));
    const auto before = stems(r);

    /* F changes on the card; nothing bakes it while the song above is not open. */
    const std::string clip = r.platform.root + "/" + kF + "/" + AudioSong::clipsDir() + "/own.wav";
    writeClip(clip, 4 * kBar, q(0.125f));
    fs::last_write_time(clip, fs::last_write_time(clip) + std::chrono::seconds(10));
    REQUIRE(elementNav().chooseOccurrence(0));
    const std::string stale = newStem(r, before, ContextState::Incomplete);
    REQUIRE_FALSE(stale.empty());
    stemIs(r, stale, 4 * kBar, q(0.3125f));                 // F's older bake, not silence
    CHECK(r.engine->contextWhy().find("older bake") != std::string::npos);
}

TEST_CASE("context: an element that grows is heard around again over its new length", "[song][element][context][nav]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElement(r, kE, 4, q(0.5f));
    openDemo(r, {{"bed.wav", 0, 12, q(0.25f)}}, {{1, kE, 4, 4}});
    enter(1, 4);
    REQUIRE(contextIs(r, ContextState::Ready));
    const auto before = stems(r);
    REQUIRE(before.size() == 1);
    stemIs(r, *before.begin(), 4 * kBar, q(0.25f));

    {
        std::lock_guard<std::recursive_mutex> l(r.engine->songMutex());
        r.engine->song().length = 6;
    }
    r.engine->commit();
    std::string longer;
    REQUIRE(waitFor([&] {
        elementNav().poll();
        if (r.engine->contextState() != ContextState::Ready) return false;
        for (const auto& f : stems(r))
            if (!before.count(f)) longer = f;
        return !longer.empty();
    }, kWaitMs));
    stemIs(r, longer, 6 * kBar, q(0.25f));
}

TEST_CASE("context: a tempo turned inside the element says the song around it is not heard, until it is put back", "[song][element][context]") {
    Rig r;
    Services svc(r.engine.get(), nullptr);
    NavGuard guard;
    writeElement(r, kE, 4, q(0.5f));
    openDemo(r, {{"bed.wav", 0, 8, q(0.25f)}}, {{1, kE, 4, 4}});
    enter(1, 4);
    REQUIRE(contextIs(r, ContextState::Ready));
    auto tempo = [&](float bpm) {
        REQUIRE(r.engine->requestTempo(bpm));
        REQUIRE(waitFor([&] { return r.engine->editState() != SongOpState::Busy; }));
        REQUIRE(r.engine->editState() == SongOpState::Ready);
    };
    tempo(100.0f);
    CHECK(contextIs(r, ContextState::Incomplete));
    CHECK(r.engine->contextWhy().find("BPM") != std::string::npos);
    tempo(120.0f);
    CHECK(contextIs(r, ContextState::Ready));
    CHECK(r.engine->contextWhy().empty());
}

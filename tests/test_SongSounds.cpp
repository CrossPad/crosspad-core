// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstring>
#include <catch2/catch_test_macros.hpp>

#include "fake_file_system.hpp"
#include "song_fakes.hpp"

#include "crosspad/instrument/InstrumentPlayState.hpp"
#include "crosspad/kit/KitSaved.hpp"
#include "crosspad/song/SongSounds.hpp"
#include "crosspad/util/RetryBackoff.hpp"

#include <filesystem>
#include <string>
#include <vector>

using namespace crosspad;

namespace {

constexpr const char* kCard = "/sdcard";
constexpr const char* kKitA = "/crosspad/kits/A/kit.json";
constexpr const char* kKitB = "/crosspad/kits/B/kit.json";
constexpr const char* kKitC = "/crosspad/kits/C/kit.json";

/// The platform side, as a list of what was asked of it.
class FakeHost final : public ISongSoundsHost {
public:
    std::string cardRoot() override { return kCard; }
    bool kitsSettled() override { return settled; }
    void loadKit(int kitId) override { loads.push_back(kitId); }
    void unloadKit() override {
        ++unloads;
        if (kits) kits->clearCurrentKit();
    }
    bool instrumentOwnsEngine() override { return owns; }
    const char* foregroundApp() override { return foreground.empty() ? nullptr : foreground.c_str(); }
    void warn(const char* msg) override { warnings.emplace_back(msg); }
    void log(bool, const char*) override {}

    IKitManager* kits = nullptr;
    bool settled = true;
    bool owns = false;
    std::string foreground;
    std::vector<int> loads;
    int unloads = 0;
    std::vector<std::string> warnings;
};

struct Rig {
    test::FakeSongEngine engine;
    PortableKitLoader kits;
    test::FakeInstrumentHost inst;
    FakeHost host;
    SongSounds sounds{host};
    test::ServicesScope scope{&engine, &kits, &inst};

    Rig() {
        host.kits = &kits;
        test::setKits(kits, {kKitA, kKitB});
        engine.song().dir = std::string(kCard) + "/SONGS/a";
        engine.song().name = "a";
    }
};

} // namespace

TEST_CASE("song sounds: an open song loads its kit, then rests", "[song][sounds]") {
    Rig r;
    r.kits.setCurrentKitId(0);
    r.engine.song().kit = kKitB;
    r.sounds.songOpened();
    r.sounds.step();
    REQUIRE(r.host.loads == std::vector<int>{1});
    REQUIRE(r.sounds.pending());

    r.kits.setCurrentKitId(1);   // the load finished
    r.sounds.step();
    REQUIRE(r.host.loads.size() == 1);
    REQUIRE_FALSE(r.sounds.pending());
    r.sounds.step();
    REQUIRE(r.host.loads.size() == 1);
}

TEST_CASE("song sounds: nothing happens before a song opens", "[song][sounds]") {
    Rig r;
    r.engine.song().kit = kKitB;
    r.sounds.step();
    REQUIRE(r.host.loads.empty());
    r.sounds.start();   // the song open before anyone listened
    r.sounds.step();
    REQUIRE(r.host.loads == std::vector<int>{1});
}

TEST_CASE("song sounds: a step waits for whatever is already moving", "[song][sounds]") {
    Rig r;
    r.engine.song().kit = kKitB;
    r.sounds.songOpened();

    r.engine.state = SongOpState::Busy;
    r.sounds.step();
    r.engine.state = SongOpState::Ready;
    r.host.settled = false;   // the kit scan, or a kit load
    r.sounds.step();
    r.host.settled = true;
    r.inst.st = IInstrumentHost::State::Busy;
    r.sounds.step();
    REQUIRE(r.host.loads.empty());
    REQUIRE(r.sounds.pending());

    r.inst.st = IInstrumentHost::State::Idle;
    r.sounds.step();
    REQUIRE(r.host.loads == std::vector<int>{1});
}

TEST_CASE("song sounds: a song without a kit leaves none loaded, not under the Sampler", "[song][sounds]") {
    Rig r;
    r.kits.setCurrentKitId(0);
    r.engine.song().tracks[1].inUse = true;   // an audio track: the song is made without a kit
    r.sounds.songOpened();
    r.host.foreground = "Sampler";
    r.sounds.step();
    REQUIRE(r.host.unloads == 0);
    REQUIRE(r.sounds.pending());

    r.host.foreground = "Arrange";
    r.sounds.step();
    REQUIRE(r.host.unloads == 1);
    REQUIRE(r.kits.getCurrentKit() == nullptr);
    REQUIRE_FALSE(r.sounds.pending());
}

TEST_CASE("song sounds: a song with nothing in it takes the kit that is loaded", "[song][sounds]") {
    Rig r;
    r.kits.setCurrentKitId(0);
    r.host.foreground = "Arrange";             // + NEW in Arrange
    r.sounds.songOpened();
    r.sounds.step();
    REQUIRE(r.host.unloads == 0);
    REQUIRE(r.kits.getCurrentKit() != nullptr);
    REQUIRE(r.engine.song().kit == kKitA);
    REQUIRE(r.engine.commits == 1);
    REQUIRE_FALSE(r.sounds.pending());
}

TEST_CASE("song sounds: a kit missing from the card is said once a song", "[song][sounds]") {
    Rig r;
    r.engine.song().kit = "/crosspad/kits/Gone/kit.json";
    r.sounds.songOpened();
    r.host.foreground = "Sampler";   // keeps the step from finishing
    r.sounds.step();
    r.sounds.step();
    REQUIRE(r.host.warnings.size() == 1);
    REQUIRE(r.host.warnings[0] == "This song's kit is not on the card: /crosspad/kits/Gone/kit.json");

    r.sounds.songOpened();   // opened again
    r.sounds.step();
    REQUIRE(r.host.warnings.size() == 2);
}

TEST_CASE("song sounds: the toast is cut to its buffer, the kit load is not", "[song][sounds]") {
    Rig r;
    r.engine.song().kit = "/crosspad/kits/" + std::string(200, 'x') + "/kit.json";
    r.sounds.songOpened();
    r.sounds.step();
    REQUIRE(r.host.warnings.size() == 1);
    REQUIRE(r.host.warnings[0].size() == 95);
}

TEST_CASE("song sounds: a song's Pitch track opens its instrument", "[song][sounds]") {
    Rig r;
    r.engine.addSeqTrack(SeqSource::Pitch);
    r.engine.song().instrument = "pitch/instrument.json";
    r.sounds.songOpened();
    r.sounds.step();
    REQUIRE(r.inst.opened == std::vector<std::string>{"/SONGS/a/pitch/instrument.json"});
    REQUIRE(r.sounds.pending());

    r.inst.st = IInstrumentHost::State::Ready;
    r.inst.inst.path = "/SONGS/a/pitch/instrument.json";
    r.inst.inst.name = "Keys";
    const uint32_t gen = instrumentPlayState().generation;
    r.sounds.step();
    REQUIRE_FALSE(r.sounds.pending());
    REQUIRE(instrumentPlayState().instrumentPath == "/SONGS/a/pitch/instrument.json");
    REQUIRE(instrumentPlayState().instrumentName == "Keys");
    REQUIRE(instrumentPlayState().generation == gen + 1);
}

TEST_CASE("song sounds: an instrument that will not load is asked for again later, said once", "[song][sounds]") {
    Rig r;
    r.engine.addSeqTrack(SeqSource::Pitch);
    r.engine.song().instrument = "/crosspad/pitchplay/Keys/instrument.json";
    r.sounds.songOpened();
    r.sounds.step();
    REQUIRE(r.inst.opened.size() == 1);
    r.inst.st = IInstrumentHost::State::Failed;            // no memory beside the kits
    r.inst.msg = "zone 0: no memory (215640 B)";
    auto stepsTo = [&](size_t opens) {                      // steps until the next try
        int n = 0;
        while (r.inst.opened.size() < opens && n < 400) { r.sounds.step(); ++n; }
        return n;
    };
    const uint32_t first = (uint32_t)stepsTo(2) * SongSounds::kStepMs;
    CHECK(first >= RetryBackoff::kFirstMs);                 // not a load a step
    CHECK(first <= RetryBackoff::kFirstMs + 2 * SongSounds::kStepMs);
    REQUIRE(r.host.warnings.size() == 1);
    CHECK(r.host.warnings[0].find("no memory") != std::string::npos);
    CHECK(r.sounds.pending());
    const uint32_t second = (uint32_t)stepsTo(3) * SongSounds::kStepMs;   // failed again
    CHECK(second >= 2 * RetryBackoff::kFirstMs);            // twice as long
    CHECK(r.host.warnings.size() == 1);                     // and not said again
    r.inst.st = IInstrumentHost::State::Ready;             // memory came back, the next try took
    r.inst.inst.path = "/crosspad/pitchplay/Keys/instrument.json";
    r.sounds.step();
    CHECK_FALSE(r.sounds.pending());
}

TEST_CASE("song sounds: an instrument from the card's root is opened as named", "[song][sounds]") {
    Rig r;
    r.engine.addSeqTrack(SeqSource::Pitch);
    r.engine.song().instrument = "/crosspad/pitchplay/Keys/instrument.json";
    r.sounds.songOpened();
    r.sounds.step();
    REQUIRE(r.inst.opened == std::vector<std::string>{"/crosspad/pitchplay/Keys/instrument.json"});
}

TEST_CASE("song sounds: an instrument without a Pitch track is not opened", "[song][sounds]") {
    Rig r;
    r.engine.addSeqTrack(SeqSource::Kit);
    r.engine.song().instrument = "pitch/instrument.json";
    r.sounds.songOpened();
    r.sounds.step();
    REQUIRE(r.inst.opened.empty());
    REQUIRE_FALSE(r.sounds.pending());
}

TEST_CASE("song sounds: the kits get the engine back, not from under the app playing it", "[song][sounds]") {
    Rig r;
    r.host.owns = true;
    for (const char* app : {"PitchPlay", "Sequencer"}) {
        r.host.foreground = app;
        r.sounds.songOpened();
        r.sounds.step();
        REQUIRE(r.inst.releases == 0);
    }
    r.host.foreground.clear();
    r.sounds.songOpened();
    r.sounds.step();
    REQUIRE(r.inst.releases == 1);
    REQUIRE(r.sounds.pending());   // the kit is compared once that is through
}

TEST_CASE("song sounds: a kit loaded while a song is open is written into it", "[song][sounds]") {
    Rig r;
    r.kits.setCurrentKitId(1);
    r.sounds.kitLoaded();
    REQUIRE(r.engine.song().kit == kKitB);
    REQUIRE(r.engine.commits == 1);
    r.sounds.kitLoaded();   // the same kit again
    REQUIRE(r.engine.commits == 1);

    r.engine.state = SongOpState::Busy;
    r.kits.setCurrentKitId(0);
    r.sounds.kitLoaded();
    REQUIRE(r.engine.song().kit == kKitB);
}

TEST_CASE("song sounds: the instrument home is the song's folder from the card's root", "[song][sounds]") {
    Rig r;
    std::string home;
    REQUIRE_FALSE(r.sounds.instrumentHome(home));
    r.engine.addSeqTrack(SeqSource::Pitch);
    REQUIRE(r.sounds.instrumentHome(home));
    REQUIRE(home == "/SONGS/a");
}

TEST_CASE("song sounds: an instrument inside the song is kept relative to it", "[song][sounds]") {
    Rig r;
    r.sounds.instrumentIs("/SONGS/a/pitch/instrument.json");
    REQUIRE(r.engine.song().instrument == "pitch/instrument.json");
    REQUIRE(r.engine.commits == 1);
    r.sounds.instrumentIs("/SONGS/a/pitch/instrument.json");
    REQUIRE(r.engine.commits == 1);
    r.sounds.instrumentIs("/crosspad/pitchplay/Keys/instrument.json");
    REQUIRE(r.engine.song().instrument == "/crosspad/pitchplay/Keys/instrument.json");
    REQUIRE(r.engine.commits == 2);
}

TEST_CASE("song sounds: an unsaved instrument is saved into the song first", "[song][sounds]") {
    Rig r;
    KitInfo unsaved;
    unsaved.name = "take";
    int saves = 0;
    std::string savedTo;
    auto save = [&](const KitInfo& s) { ++saves; savedTo = s.path; return true; };

    r.sounds.keepInstrument(unsaved, save);   // no Pitch track: browsing keeps nothing
    REQUIRE(saves == 0);
    REQUIRE(r.engine.song().instrument.empty());

    r.engine.addSeqTrack(SeqSource::Pitch);
    r.sounds.keepInstrument(unsaved, save);
    REQUIRE(saves == 1);
    REQUIRE(savedTo == "/SONGS/a/pitch/instrument.json");
    REQUIRE(r.engine.song().instrument == "pitch/instrument.json");

    KitInfo named;
    named.path = "/crosspad/pitchplay/Keys/instrument.json";
    r.sounds.keepInstrument(named, save);
    REQUIRE(saves == 1);
    REQUIRE(r.engine.song().instrument == named.path);
}

TEST_CASE("song sounds: an instrument opened while the song's sounds are still coming is not kept", "[song][sounds]") {
    /* Opening a song, a sound keeper still on the last song's instrument asked
     * for it before this step asked for the song's own -- and its open,
     * kept, wrote the last song's instrument over this one's (28.10.2026). */
    Rig r;
    r.engine.addSeqTrack(SeqSource::Pitch);
    r.engine.song().instrument = "/crosspad/pitchplay/sub_bass/instrument.json";
    r.sounds.songOpened();
    REQUIRE(r.sounds.pending());
    KitInfo stale;
    stale.path = "/crosspad/pitchplay/BASS_Moog/instrument.json";
    r.sounds.keepInstrument(stale, nullptr);
    REQUIRE(r.engine.song().instrument == "/crosspad/pitchplay/sub_bass/instrument.json");

    /* Built from edits, it is the player's sound whenever it comes. */
    r.sounds.keepInstrument(stale, nullptr, true);
    REQUIRE(r.engine.song().instrument == stale.path);
}

TEST_CASE("song sounds: the song's pad map goes onto the pads, and a new one Pitch Play publishes into the song", "[song][sounds][pitchpads]") {
    Rig r;
    r.engine.addSeqTrack(SeqSource::Pitch);
    r.engine.song().instrument = "/crosspad/pitchplay/sub/instrument.json";
    AudioSong::PitchPads pads;
    pads.set = true;
    pads.octave = -1;
    pads.transpose = 3;
    for (int i = 0; i < 16; ++i) pads.note[i] = (uint8_t)(24 + i);
    std::memset(pads.chord, 0xFF, sizeof pads.chord);
    r.engine.song().pitchPads = pads;
    r.sounds.songOpened();
    r.sounds.step();
    r.inst.st = IInstrumentHost::State::Ready;
    r.inst.inst.path = "/crosspad/pitchplay/sub/instrument.json";
    r.sounds.step();
    REQUIRE_FALSE(r.sounds.pending());
    InstrumentPlayState& ps = instrumentPlayState();
    CHECK(ps.play.padMidiMap[0] == 24);
    CHECK(ps.play.octave == -1);
    CHECK(ps.transpose == 3);

    const int commits = r.engine.commits;
    r.sounds.step();                                   // nothing new published: nothing written
    CHECK(r.engine.commits == commits);

    ps.play.padMidiMap[0] = 36;                        // Pitch Play, an octave up
    ps.play.octave = 0;
    instrumentPlayStatePublished();
    r.sounds.step();
    CHECK(r.engine.song().pitchPads.note[0] == 36);
    CHECK(r.engine.song().pitchPads.octave == 0);
    CHECK(r.engine.commits == commits + 1);
}

TEST_CASE("song sounds: a save that fails keeps nothing", "[song][sounds]") {
    Rig r;
    r.engine.addSeqTrack(SeqSource::Pitch);
    r.sounds.keepInstrument(KitInfo{}, [](const KitInfo&) { return false; });
    REQUIRE(r.engine.song().instrument.empty());
    REQUIRE(r.engine.commits == 0);
    r.sounds.keepInstrument(KitInfo{}, nullptr);   // a host that cannot save
    REQUIRE(r.engine.song().instrument.empty());
}

TEST_CASE("song sounds: no song engine or kit manager ends the wait", "[song][sounds]") {
    test::FakeInstrumentHost inst;
    FakeHost host;
    SongSounds sounds{host};
    test::ServicesScope scope{nullptr, nullptr, &inst};
    sounds.songOpened();
    sounds.step();
    REQUIRE_FALSE(sounds.pending());
}

TEST_CASE("song sounds: a kit picked while another loads is the one the song keeps", "[song][sounds]") {
    Rig r;
    test::setKits(r.kits, {kKitA, kKitB, kKitC});
    r.kits.setCurrentKitId(0);
    r.engine.song().kit = kKitB;
    r.sounds.songOpened();
    r.sounds.step();                       // the song's kit, B, starts loading
    REQUIRE(r.host.loads == std::vector<int>{1});

    r.engine.song().kit = kKitC;           // picked in Arrange while B loads
    r.kits.setCurrentKitId(1);             // B is in
    r.sounds.kitLoaded();
    REQUIRE(r.engine.song().kit == kKitC); // the newer pick is not written over
    REQUIRE(r.engine.commits == 0);

    r.sounds.step();
    REQUIRE(r.host.loads == std::vector<int>{1, 2});
    r.kits.setCurrentKitId(2);
    r.sounds.kitLoaded();
    r.sounds.step();
    REQUIRE(r.engine.song().kit == kKitC);
    REQUIRE_FALSE(r.sounds.pending());
}

TEST_CASE("song sounds: a kit given to the song after it settled is loaded", "[song][sounds]") {
    Rig r;
    r.kits.setCurrentKitId(0);
    r.engine.song().kit = kKitA;
    r.sounds.songOpened();
    r.sounds.step();
    REQUIRE_FALSE(r.sounds.pending());
    REQUIRE(r.host.loads.empty());

    r.engine.song().kit = kKitB;           // a kit track given its kit in Arrange
    r.sounds.step();
    r.sounds.step();
    REQUIRE(r.host.loads == std::vector<int>{1});
}

TEST_CASE("song sounds: a kit loaded from the browser is still the song's", "[song][sounds]") {
    Rig r;
    r.kits.setCurrentKitId(0);
    r.engine.song().kit = kKitA;
    r.sounds.songOpened();
    r.sounds.step();
    r.kits.setCurrentKitId(1);             // picked in the kit browser
    r.sounds.kitLoaded();
    REQUIRE(r.engine.song().kit == kKitB);
    r.sounds.step();
    r.sounds.step();
    REQUIRE(r.host.loads.empty());         // and not taken back to A
}

TEST_CASE("song sounds: the live kit track's kit is loaded, and a kit loaded is that track's", "[song][sounds]") {
    Rig r;
    test::setKits(r.kits, {kKitA, kKitB, kKitC});
    r.kits.setCurrentKitId(0);
    auto& song = r.engine.song();
    song.kit = kKitA;
    REQUIRE(song.addSeqTrack(0, SeqSource::Kit, 0));       // the song's kit, A
    REQUIRE(song.addSeqTrack(1, SeqSource::Kit, 1, kKitB));
    r.sounds.songOpened();
    r.sounds.step();
    REQUIRE(r.host.loads.empty());                         // A is in

    REQUIRE(song.setLiveKit(1));                           // Arrange hands the engine over
    r.sounds.step();
    r.sounds.step();
    REQUIRE(r.host.loads == std::vector<int>{1});
    r.kits.setCurrentKitId(1);
    r.sounds.kitLoaded();
    r.sounds.step();
    REQUIRE(r.host.loads.size() == 1);

    /* Another kit loaded (the Sampler's browser, from the track's seq view):
     * the live track's, not the other one's. */
    r.kits.setCurrentKitId(2);
    r.sounds.kitLoaded();
    CHECK(song.tracks[1].kit == kKitC);
    CHECK(song.tracks[0].kit == kKitA);
    CHECK(song.kit == kKitC);
}

TEST_CASE("kit saved: a kit written by the loader is announced under its logical path", "[kit][sounds]") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "crosspad_kit_saved_test";
    fs::remove_all(root);
    fs::create_directories(root / "crosspad/kits/Saved");
    test::FakeFileSystem files;
    test::UseFileSystem scope(files);
    PortableKitLoader kits(root.string());
    KitInfo kit;
    kit.path = "/crosspad/kits/Saved/kit.json";
    kit.name = "Saved";

    const uint32_t before = kit_saved_generation(kit.path);
    REQUIRE(kits.saveKit(kit));
    CHECK(kit_saved_generation(kit.path) == before + 1);
    CHECK(kit_saved_generation(root.string() + kit.path) == 0);   // never under the file system's

    fs::remove_all(root);
}

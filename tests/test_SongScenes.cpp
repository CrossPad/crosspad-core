// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "song_fakes.hpp"

#include "crosspad/kit/KitPathUtils.hpp"
#include "crosspad/sequencer/MetronomeNode.hpp"
#include "crosspad/sequencer/PatternSequencer.hpp"
#include "crosspad/sequencer/SequenceStore.hpp"
#include "crosspad/song/SongScenes.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

using namespace crosspad;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kRate  = 48000;
constexpr uint32_t kBlock = 256;

/// A card of its own in the temp folder, gone after the test.
struct Card {
    fs::path root;
    explicit Card(const char* name) : root(fs::temp_directory_path() / name) {
        fs::remove_all(root);
        fs::create_directories(root / "crosspad");
    }
    ~Card() { std::error_code ec; fs::remove_all(root, ec); }
    std::string str() const { return root.string(); }
    std::string song(const char* name) const {
        fs::create_directories(root / "SONGS" / name);
        return (root / "SONGS" / name).string();
    }
    std::string legacy() const { return str() + SongScenes::kLegacyFile; }
    std::string mark() const { return str() + SongScenes::kAdoptedMark; }
};

struct Rig {
    MetronomeNode clock;
    PatternSequencer seq;
    test::FakeSongEngine engine;
    SongScenes scenes{seq, nullptr};
    test::ServicesScope scope{&engine, nullptr, nullptr};

    Rig() {
        clock.onPrepare(kBlock, kRate);
        clock.setBpm(120.0f);
        seq.attach(&clock);
        seq.resetScenes();
    }
    void open(const std::string& dir, bool seqTrack = false) {
        engine.song() = AudioSong{};
        engine.song().dir = dir;
        if (seqTrack) engine.addSeqTrack(SeqSource::Kit);
    }
};

/// A scenes file at @p path holding one hit on @p pad.
void writeScenes(const std::string& path, uint8_t pad)
{
    MetronomeNode clock;
    clock.onPrepare(kBlock, kRate);
    PatternSequencer seq;
    seq.attach(&clock);
    seq.resetScenes();
    REQUIRE(seq.addEventTicks(0, 0, pad, 100));
    REQUIRE(SequenceStore::save(seq, path));
}

/// The pad of the first hit in @p path's first scene; -1 for none.
int firstPad(const std::string& path)
{
    MetronomeNode clock;
    clock.onPrepare(kBlock, kRate);
    PatternSequencer seq;
    seq.attach(&clock);
    if (!SequenceStore::load(seq, path)) return -1;
    uint32_t ticks = 0;
    uint8_t pad = 0, vel = 0;
    return seq.eventAtTicks(0, 0, ticks, pad, vel) ? pad : -1;
}

} // namespace

TEST_CASE("song scenes: the first song adopts the old shared file, and saves its own", "[song][scenes]") {
    Card card("cp_scenes_first");
    writeScenes(card.legacy(), 7);
    Rig r;
    const std::string a = card.song("a");
    r.open(a);

    r.scenes.sweep(card.str());
    REQUIRE(r.scenes.currentFile() == a + "/sequences.json");
    REQUIRE(r.seq.eventCount() == 1);
    REQUIRE(r.seq.dirty());
    REQUIRE(fs::exists(card.mark()));

    r.scenes.sweep(card.str());   // the adopted scenes go into the song
    REQUIRE_FALSE(r.seq.dirty());
    REQUIRE(firstPad(a + "/sequences.json") == 7);
    REQUIRE(fs::exists(card.legacy()));   // read, never moved or written
}

TEST_CASE("song scenes: a take played before the first song opened is that song's", "[song][scenes]") {
    Card card("cp_scenes_early_take");
    writeScenes(card.legacy(), 7);
    Rig r;
    REQUIRE(r.seq.addEventTicks(0, 0, 5, 100));
    r.seq.markDirty();
    const std::string a = card.song("a");
    r.open(a);

    r.scenes.sweep(card.str());
    REQUIRE(r.scenes.currentFile() == a + "/sequences.json");
    REQUIRE(r.seq.eventCount() == 1);
    REQUIRE(r.seq.dirty());

    r.scenes.sweep(card.str());   // the take goes into the song, not the old shared file
    REQUIRE_FALSE(r.seq.dirty());
    REQUIRE(firstPad(a + "/sequences.json") == 5);
}

TEST_CASE("song scenes: after the first, only a song with a seq track adopts", "[song][scenes]") {
    Card card("cp_scenes_later");
    writeScenes(card.legacy(), 7);
    { std::FILE* f = std::fopen(card.mark().c_str(), "w"); REQUIRE(f); std::fclose(f); }
    Rig r;

    r.open(card.song("plain"));
    r.scenes.sweep(card.str());
    REQUIRE(r.seq.eventCount() == 0);
    REQUIRE_FALSE(r.seq.dirty());

    r.open(card.song("grooves"), true);
    r.scenes.sweep(card.str());
    REQUIRE(r.seq.eventCount() == 1);
    REQUIRE(r.seq.dirty());
}

TEST_CASE("song scenes: two songs keep their own grooves", "[song][scenes]") {
    Card card("cp_scenes_two");
    const std::string a = card.song("a");
    const std::string b = card.song("b");
    writeScenes(a + "/sequences.json", 3);
    writeScenes(b + "/sequences.json", 9);
    Rig r;

    r.open(a);
    r.scenes.sweep(card.str());
    REQUIRE(r.scenes.currentFile() == a + "/sequences.json");
    uint32_t ticks = 0;
    uint8_t pad = 0, vel = 0;
    REQUIRE(r.seq.eventAtTicks(0, 0, ticks, pad, vel));
    REQUIRE(pad == 3);

    r.open(b);
    r.scenes.sweep(card.str());
    REQUIRE(r.scenes.currentFile() == b + "/sequences.json");
    REQUIRE(r.seq.eventAtTicks(0, 0, ticks, pad, vel));
    REQUIRE(pad == 9);
    REQUIRE(firstPad(a + "/sequences.json") == 3);
}

TEST_CASE("song scenes: a switch while dirty saves the last song's first", "[song][scenes]") {
    Card card("cp_scenes_dirty");
    const std::string a = card.song("a");
    const std::string b = card.song("b");
    writeScenes(b + "/sequences.json", 9);
    { std::FILE* f = std::fopen(card.mark().c_str(), "w"); REQUIRE(f); std::fclose(f); }
    Rig r;

    r.open(a);
    r.scenes.sweep(card.str());   // a has none yet
    REQUIRE(r.seq.addEventTicks(0, 0, 5, 100));
    r.seq.markDirty();

    r.open(b);
    r.scenes.sweep(card.str());
    REQUIRE(firstPad(a + "/sequences.json") == 5);
    REQUIRE(r.scenes.currentFile() == b + "/sequences.json");
    REQUIRE(firstPad(b + "/sequences.json") == 9);
}

TEST_CASE("song scenes: a song whose folder went keeps nothing behind", "[song][scenes]") {
    Card card("cp_scenes_gone");
    const std::string a = card.song("a");
    { std::FILE* f = std::fopen(card.mark().c_str(), "w"); REQUIRE(f); std::fclose(f); }
    Rig r;
    r.open(a);
    r.scenes.sweep(card.str());
    REQUIRE(r.seq.addEventTicks(0, 0, 5, 100));
    r.seq.markDirty();
    fs::remove_all(a);   // deleted, or renamed

    r.open(card.song("b"));
    r.scenes.sweep(card.str());
    REQUIRE_FALSE(fs::exists(a));
}

TEST_CASE("song scenes: an unreadable file is neither shown nor written over", "[song][scenes]") {
    Card card("cp_scenes_bad");
    const std::string a = card.song("a");
    const std::string file = a + "/sequences.json";
    { std::FILE* f = std::fopen(file.c_str(), "w"); REQUIRE(f); std::fputs("{ not json", f); std::fclose(f); }
    Rig r;
    REQUIRE(r.seq.addEventTicks(0, 0, 5, 100));
    r.open(a);
    r.scenes.sweep(card.str());
    REQUIRE(r.scenes.currentFile().empty());
    REQUIRE(r.seq.eventCount() == 0);
    r.seq.markDirty();
    r.scenes.saveSoon();
    r.scenes.sweep(card.str());
    std::FILE* f = std::fopen(file.c_str(), "r");
    REQUIRE(f);
    char head[16] = {};
    REQUIRE(std::fgets(head, sizeof(head), f));
    std::fclose(f);
    REQUIRE(std::string(head) == "{ not json");
}

TEST_CASE("song scenes: changed scenes land, a requested write lands clean ones", "[song][scenes]") {
    Card card("cp_scenes_save");
    const std::string a = card.song("a");
    { std::FILE* f = std::fopen(card.mark().c_str(), "w"); REQUIRE(f); std::fclose(f); }
    Rig r;
    r.open(a);
    r.scenes.sweep(card.str());
    r.scenes.sweep(card.str());
    REQUIRE_FALSE(fs::exists(a + "/sequences.json"));   // nothing changed

    r.scenes.saveSoon();
    r.scenes.sweep(card.str());
    REQUIRE(fs::exists(a + "/sequences.json"));

    REQUIRE(r.seq.addEventTicks(0, 0, 4, 100));
    r.seq.markDirty();
    r.engine.bounce.state = BounceState::Running;   // not mid-bounce
    r.scenes.sweep(card.str());
    REQUIRE(r.seq.dirty());
    REQUIRE(firstPad(a + "/sequences.json") == -1);
    r.engine.bounce.state = BounceState::Idle;
    r.scenes.sweep(card.str());
    REQUIRE_FALSE(r.seq.dirty());
    REQUIRE(firstPad(a + "/sequences.json") == 4);
}

TEST_CASE("song scenes: a card that never had shared scenes adopts the kit's", "[song][scenes]") {
    Card card("cp_scenes_kit");
    fs::create_directories(card.root / "kits" / "K");
    writeScenes(card.str() + "/kits/K/sequences.json", 11);
    struct Prefix {
        explicit Prefix(const std::string& p) { setKitPathFsPrefix(p); }
        ~Prefix() { setKitPathFsPrefix(""); }
    } prefix(card.str());
    Rig r;
    r.scenes.kitChanged("/kits/K/kit.json");   // as the kit list names it
    r.open(card.song("a"));
    r.scenes.sweep(card.str());
    REQUIRE(r.seq.eventCount() == 1);
    REQUIRE(r.seq.dirty());
}

TEST_CASE("song scenes: no song open, nothing read", "[song][scenes]") {
    Card card("cp_scenes_none");
    writeScenes(card.legacy(), 7);
    Rig r;
    r.engine.state = SongOpState::Busy;
    r.scenes.sweep(card.str());
    REQUIRE(r.scenes.currentFile().empty());
    REQUIRE(r.seq.eventCount() == 0);
}

TEST_CASE("song scenes: a platform without a song engine keeps the one file", "[song][scenes]") {
    Card card("cp_scenes_nosongs");
    writeScenes(card.legacy(), 7);
    MetronomeNode clock;
    clock.onPrepare(kBlock, kRate);
    PatternSequencer seq;
    seq.attach(&clock);
    SongScenes scenes{seq, nullptr};
    test::ServicesScope scope{nullptr, nullptr, nullptr};
    scenes.sweep(card.str());
    REQUIRE(scenes.currentFile() == card.legacy());
    REQUIRE(seq.eventCount() == 1);
}

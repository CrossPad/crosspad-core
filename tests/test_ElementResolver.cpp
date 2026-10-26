// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "crosspad/song/BakedView.hpp"
#include "crosspad/song/SeqFreezeKey.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace crosspad;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kRate = 48000;
const char* const kA = ".elements/E00000001";
const char* const kB = ".elements/E00000002";
const char* const kC = ".elements/E00000003";

/// The card as a folder under the temp root.
struct Card {
    std::string root;
    explicit Card(const char* name) {
        root = (fs::temp_directory_path() / "cp_core_tests" / name).string();
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~Card() { fs::remove_all(root); }

    ElementHashReader reader() const {
        ElementHashReader rd;
        rd.info = [](const std::string& p, uint64_t& bytes, int64_t& modified) {
            std::error_code ec;
            if (!fs::is_regular_file(p, ec)) return false;
            bytes = fs::file_size(p, ec);
            modified = 0;   // sizes tell the files apart; a time would make the test flaky
            return true;
        };
        rd.read = [](const std::string& p, std::string& bytes) {
            std::ifstream in(p, std::ios::binary);
            if (!in) return false;
            std::ostringstream ss;
            ss << in.rdbuf();
            bytes = ss.str();
            return true;
        };
        return rd;
    }

    /// An element of one recording of @p size bytes and a block of each of @p uses after it.
    void write(const std::string& ref, size_t size, const std::vector<std::string>& uses = {}) {
        AudioSong s;
        s.dir = root + "/" + ref;
        fs::create_directories(fs::path(s.dir) / AudioSong::clipsDir());
        s.bpm = 120.0f;
        s.tracks[0].inUse = true;
        SongClip c;
        c.file = "r.wav";
        c.bars = 2;
        s.addClip(0, c);
        std::ofstream(s.clipPath("r.wav"), std::ios::binary) << std::string(size, 'x');
        int t = 1;
        for (const std::string& u : uses) {
            s.tracks[t].inUse = true;
            SongClip e;
            e.element = u;
            e.bars = 2;
            s.addClip(t++, e);
        }
        REQUIRE(s.save());
    }
};

/// A seq track on @p t of @p s playing its own notes, frozen to f.wav as the
/// engine would have stamped it.
void frozenSeqTrack(AudioSong& s, int t) {
    REQUIRE(s.addSeqTrack(t, SeqSource::Kit, 0));
    SeqNote n;
    n.pad = 1;
    n.vel = 100;
    s.putSeqTake(t, 0, makeSeqPattern(1, {n}), s.samplesPerBar(kRate), 2);
    SongClip fz;
    fz.file = "f.wav";
    fz.bars = 2;
    s.tracks[t].freeze = {fz};
    s.tracks[t].freezeKey = seq_freeze_key(s, t, kRate, SceneLookup{});
    std::ofstream(s.clipPath("f.wav")) << "frozen";
}

} // namespace

TEST_CASE("resolver: a change two levels down changes the direct child's key", "[song][resolver]") {
    Card card("cp_resolver_nest");
    card.write(kC, 100);
    card.write(kB, 50, {kC});
    card.write(kA, 70, {kB});
    ElementResolver before(card.root, kRate, 120.0f, card.reader());
    const uint64_t b = before.resolve(kB).key;
    const uint64_t a = before.resolve(kA).key;
    CHECK(before.resolve(kB).key == b);
    CHECK(before.resolve(kB).file == element_bake_file(b));

    card.write(kC, 101);   // only the grandchild changed
    ElementResolver after(card.root, kRate, 120.0f, card.reader());
    CHECK(after.resolve(kB).key != b);
    CHECK(after.resolve(kA).key != a);
    CHECK(after.resolve(kC).key != before.resolve(kC).key);
}

TEST_CASE("resolver: the bake on the card is found, one that is not is bakeable from frozen files", "[song][resolver]") {
    Card card("cp_resolver_exists");
    card.write(kC, 100);
    ElementResolver first(card.root, kRate, 120.0f, card.reader());
    const ResolvedElement& r = first.resolve(kC);
    CHECK_FALSE(r.exists);
    CHECK(r.bakeable);
    CHECK(r.why.empty());
    CHECK(r.frames == 2 * 2 * kRate);

    fs::create_directories(card.root + "/" + AudioSong::bakeDir());
    std::ofstream(card.root + "/" + AudioSong::bakeDir() + "/" + r.file) << "baked";
    ElementResolver second(card.root, kRate, 120.0f, card.reader());
    CHECK(second.resolve(kC).exists);
}

TEST_CASE("resolver: an unfrozen seq track makes an element, and the one above it, not bakeable", "[song][resolver]") {
    Card card("cp_resolver_unfrozen");
    card.write(kC, 100);
    AudioSong s;
    REQUIRE(s.load(card.root + "/" + kC) == SongLoad::Loaded);
    s.tracks[3].inUse = true;
    s.tracks[3].kind = TrackKind::Seq;
    REQUIRE(s.save());
    card.write(kB, 50, {kC});
    // write() rewrote kB only; kC keeps its seq track
    ElementResolver rs(card.root, kRate, 120.0f, card.reader());
    CHECK_FALSE(rs.resolve(kC).bakeable);
    CHECK(rs.resolve(kC).why.find("Track 4") != std::string::npos);
    CHECK(rs.resolve(kC).rebakes);                         // entered and left, it is frozen
    CHECK_FALSE(rs.resolve(kB).bakeable);
    CHECK(rs.resolve(kB).why.find(kC) != std::string::npos);
    CHECK_FALSE(rs.resolve(kB).rebakes);                   // its child's to mend, not its own
}

TEST_CASE("resolver: a stretched track not fitted yet, or a freeze at another tempo, is not bakeable", "[song][resolver]") {
    Card card("cp_resolver_tempo");
    card.write(kC, 100);
    AudioSong s;
    REQUIRE(s.load(card.root + "/" + kC) == SongLoad::Loaded);
    s.tracks[0].stretch = StretchMode::KeepPitch;
    s.tracks[0].clips[0].frames = 2 * 2 * kRate / 2;   // 2:1 against its two bars
    REQUIRE(s.save());
    ElementResolver unfitted(card.root, kRate, 120.0f, card.reader());
    CHECK_FALSE(unfitted.resolve(kC).bakeable);
    CHECK(unfitted.resolve(kC).why.find("not fitted") != std::string::npos);
    const SongClip& c = s.tracks[0].clips[0];
    const std::string copy = s.clipPath(stretchName(c, 2 * 2 * kRate, StretchMode::KeepPitch));
    fs::create_directories(fs::path(copy).parent_path());
    std::ofstream(copy) << "fitted";
    ElementResolver fitted(card.root, kRate, 120.0f, card.reader());
    CHECK(fitted.resolve(kC).bakeable);

    frozenSeqTrack(s, 3);
    REQUIRE(s.save());
    ElementResolver same(card.root, kRate, 120.0f, card.reader());
    CHECK(same.resolve(kC).bakeable);
    ElementResolver other(card.root, kRate, 100.0f, card.reader());
    CHECK_FALSE(other.resolve(kC).bakeable);
    CHECK(other.resolve(kC).why.find("another tempo") != std::string::npos);
    CHECK(other.resolve(kC).rebakes);
}

TEST_CASE("resolver: a recording that keeps its speed is not bakeable at another tempo", "[song][resolver]") {
    Card card("cp_resolver_unstretched");
    card.write(kC, 100);                                     // at 120, StretchMode::Off
    ElementResolver same(card.root, kRate, 120.0f, card.reader());
    CHECK(same.resolve(kC).bakeable);
    ElementResolver other(card.root, kRate, 100.0f, card.reader());
    CHECK_FALSE(other.resolve(kC).bakeable);
    CHECK(other.resolve(kC).why.find("not fitted") != std::string::npos);
}

TEST_CASE("resolver: a freeze older than its notes, or with no stamp, is not bakeable", "[song][resolver]") {
    Card card("cp_resolver_freeze_key");
    card.write(kC, 100);
    AudioSong s;
    REQUIRE(s.load(card.root + "/" + kC) == SongLoad::Loaded);
    frozenSeqTrack(s, 1);
    REQUIRE(s.save());
    CHECK(ElementResolver(card.root, kRate, 120.0f, card.reader()).resolve(kC).bakeable);

    SECTION("notes edited and never frozen again") {
        SeqNote n;
        n.pad = 2;
        n.vel = 90;
        s.putSeqTake(1, 0, makeSeqPattern(1, {n}), s.samplesPerBar(kRate), 2);
    }
    SECTION("the stamp cleared (a kit picked since)") {
        s.tracks[1].freezeKey = 0;
    }
    SECTION("a block playing the scene, which no sequencer here can tell") {
        s.tracks[1].clips[0].notes = nullptr;
        s.tracks[1].freezeKey = seq_freeze_key(s, 1, kRate, SceneLookup{});
        CHECK(s.tracks[1].freezeKey == 0);
    }
    REQUIRE(s.save());
    ElementResolver rs(card.root, kRate, 120.0f, card.reader());
    CHECK_FALSE(rs.resolve(kC).bakeable);
    CHECK(rs.resolve(kC).why.find("older than its notes") != std::string::npos);
}

TEST_CASE("resolver: two elements with takes alike in name, size and time have two keys", "[song][resolver]") {
    Card card("cp_resolver_identity");
    card.write(kA, 100);
    card.write(kB, 100);
    std::ofstream(card.root + "/" + kB + "/" + AudioSong::clipsDir() + "/r.wav", std::ios::binary) << std::string(100, 'y');
    ElementResolver rs(card.root, kRate, 120.0f, card.reader());   // the reader gives every file one time
    CHECK(rs.resolve(kA).key != rs.resolve(kB).key);
}

TEST_CASE("resolver: a cycle is reported, not followed", "[song][resolver]") {
    Card card("cp_resolver_cycle");
    card.write(kA, 10, {kB});
    card.write(kB, 10, {kA});
    ElementResolver rs(card.root, kRate, 120.0f, card.reader());
    const ResolvedElement& a = rs.resolve(kA);
    CHECK_FALSE(a.bakeable);
    CHECK_FALSE(a.exists);
    CHECK(a.why.find("cycle") != std::string::npos);
    CHECK_FALSE(a.rebakes);
}

TEST_CASE("element hash: with a childKey an element block is hashed by it, without by its stored file", "[song][hash]") {
    AudioSong s;
    s.dir = "/nowhere";
    s.tracks[0].inUse = true;
    SongClip e;
    e.element = kB;
    e.file = "Bstored.wav";
    e.bars = 1;
    s.addClip(0, e);
    ElementHashReader rd;
    const uint64_t stage1 = element_content_hash(s, kB, rd);
    uint64_t key = 1;
    rd.childKey = [&](const std::string&, uint64_t& k) { k = key; return true; };
    const uint64_t one = element_content_hash(s, kB, rd);
    CHECK(one != stage1);
    key = 2;
    CHECK(element_content_hash(s, kB, rd) != one);
    s.tracks[0].clips[0].file = "Bother.wav";
    key = 1;
    CHECK(element_content_hash(s, kB, rd) == one);
}

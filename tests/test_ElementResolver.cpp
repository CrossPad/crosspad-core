// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "crosspad/song/ElementResolver.hpp"

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
    CHECK_FALSE(rs.resolve(kB).bakeable);
    CHECK(rs.resolve(kB).why.find(kC) != std::string::npos);
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
    const uint64_t stage1 = element_content_hash(s, rd);
    uint64_t key = 1;
    rd.childKey = [&](const std::string&, uint64_t& k) { k = key; return true; };
    const uint64_t one = element_content_hash(s, rd);
    CHECK(one != stage1);
    key = 2;
    CHECK(element_content_hash(s, rd) != one);
    s.tracks[0].clips[0].file = "Bother.wav";
    key = 1;
    CHECK(element_content_hash(s, rd) == one);
}

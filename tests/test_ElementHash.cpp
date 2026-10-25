// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "crosspad/song/ElementHash.hpp"
#include "crosspad/kit/KitPathUtils.hpp"

#include <filesystem>
#include <map>
#include <string>

using namespace crosspad;

namespace {

/// A card in memory: file name -> contents, with a size and a time each.
struct FakeCard {
    std::map<std::string, std::string> files;
    std::map<std::string, int64_t> modified;

    ElementHashReader reader() {
        ElementHashReader rd;
        rd.info = [this](const std::string& path, uint64_t& bytes, int64_t& when) {
            const auto it = files.find(path);
            if (it == files.end()) return false;
            bytes = it->second.size();
            const auto m = modified.find(path);
            when = m == modified.end() ? 0 : m->second;
            return true;
        };
        rd.read = [this](const std::string& path, std::string& bytes) {
            const auto it = files.find(path);
            if (it == files.end()) return false;
            bytes = it->second;
            return true;
        };
        rd.list = [this](const std::string& dir, const std::function<void(const std::string&)>& each) {
            for (const auto& f : files)
                if (f.first.compare(0, dir.size() + 1, dir + "/") == 0) each(f.first);
        };
        return rd;
    }
};

AudioSong baseSong() {
    AudioSong s;
    s.dir = "/songs/.elements/E00000001";
    s.bpm = 120.0f;
    s.tracks[0].inUse = true;
    SongClip c;
    c.file = "a.wav";
    c.bar = 0;
    c.bars = 2;
    s.addClip(0, c);
    return s;
}

FakeCard baseCard() {
    FakeCard card;
    card.files["/songs/.elements/E00000001/clips/a.wav"] = std::string(100, 'x');
    return card;
}

uint64_t hashOf(const AudioSong& s, FakeCard& card) { return element_content_hash(s, card.reader()); }

} // namespace

TEST_CASE("element hash: a note, a volume or a recording changes it; bpm, name and title do not", "[element][hash]") {
    FakeCard card = baseCard();
    const AudioSong base = baseSong();
    const uint64_t h = hashOf(base, card);
    CHECK(hashOf(base, card) == h);

    AudioSong other = base;
    other.bpm = 90.0f;
    other.name = "renamed";
    other.title = "a title";
    other.armed = 2;
    CHECK(hashOf(other, card) == h);

    other = base;
    other.tracks[0].volume = 0.5f;
    CHECK(hashOf(other, card) != h);

    other = base;
    other.tracks[0].clips[0].bar = 1;
    CHECK(hashOf(other, card) != h);

    other = base;
    other.tracks[1].inUse = true;
    other.tracks[1].kind = TrackKind::Seq;
    const uint64_t withSeq = hashOf(other, card);
    other.tracks[1].clips.push_back(SongClip{});
    other.tracks[1].clips[0].bars = 1;
    other.tracks[1].clips[0].notes = makeSeqPattern(1, {SeqNote{0, 24, 3, 100, 0}});
    const uint64_t oneNote = hashOf(other, card);
    CHECK(oneNote != withSeq);
    other.tracks[1].clips[0].notes = makeSeqPattern(1, {SeqNote{0, 24, 4, 100, 0}});
    CHECK(hashOf(other, card) != oneNote);

    FakeCard rerecorded = baseCard();
    rerecorded.files["/songs/.elements/E00000001/clips/a.wav"] = std::string(120, 'x');
    CHECK(hashOf(base, rerecorded) != h);
    FakeCard touched = baseCard();
    touched.modified["/songs/.elements/E00000001/clips/a.wav"] = 5;
    CHECK(hashOf(base, touched) != h);
}

TEST_CASE("element hash: a silenced track's content does not count", "[element][hash]") {
    FakeCard card = baseCard();
    AudioSong a = baseSong();
    a.tracks[1].inUse = true;
    a.tracks[1].muted = true;
    SongClip c;
    c.file = "b.wav";
    c.bars = 1;
    a.addClip(1, c);
    AudioSong b = a;
    b.tracks[1].clips[0].file = "other.wav";
    b.tracks[1].volume = 0.3f;
    CHECK(hashOf(a, card) == hashOf(b, card));
    b.tracks[1].muted = false;
    CHECK(hashOf(a, card) != hashOf(b, card));
}

TEST_CASE("element hash: the kit's files and the instrument's are in it", "[element][hash]") {
    FakeCard card = baseCard();
    card.files["/crosspad/kits/K/kit.json"] = "{}";
    card.files["/crosspad/kits/K/kick.wav"] = "kick";
    AudioSong s = baseSong();
    s.kit = "/crosspad/kits/K/kit.json";
    const uint64_t h = hashOf(s, card);
    FakeCard edited = card;
    edited.files["/crosspad/kits/K/kit.json"] = "{ }";
    CHECK(hashOf(s, edited) != h);
    FakeCard swapped = card;
    swapped.files["/crosspad/kits/K/kick.wav"] = "kicks";
    CHECK(hashOf(s, swapped) != h);

    AudioSong p = baseSong();
    p.instrument = "pitch/instrument.json";
    const uint64_t hp = hashOf(p, card);
    FakeCard inst = card;
    inst.files["/songs/.elements/E00000001/pitch/instrument.json"] = "{}";
    CHECK(hashOf(p, inst) != hp);
}

TEST_CASE("element hash: a nested element counts by the bake it plays", "[element][hash]") {
    FakeCard card = baseCard();
    AudioSong s = baseSong();
    SongClip e;
    e.element = ".elements/E00000002";
    e.file = "B0000000000000001.wav";
    e.frames = 1000;
    e.bars = 2;
    s.addClip(0, e);
    const uint64_t h = hashOf(s, card);
    s.tracks[0].clips.back().file = "B0000000000000002.wav";
    CHECK(hashOf(s, card) != h);
}

TEST_CASE("element bake key: tempo and rate are in it", "[element][hash]") {
    CHECK(element_bake_key(1, 120.0f, 44100) != element_bake_key(1, 100.0f, 44100));
    CHECK(element_bake_key(1, 120.0f, 44100) != element_bake_key(1, 120.0f, 48000));
    CHECK(element_bake_key(1, 120.0f, 44100) != element_bake_key(2, 120.0f, 44100));
    CHECK(element_bake_key(1, 120.0f, 44100) == element_bake_key(1, 120.0f, 44100));
    CHECK(element_bake_file(0x0123456789ABCDEFull) == "B0123456789ABCDEF.wav");
}

TEST_CASE("element hash: the same after song.json is saved and read back", "[element][hash]") {
    const std::string dir = (std::filesystem::temp_directory_path() / "cp_element_hash_roundtrip").string();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    AudioSong s = baseSong();
    s.dir = dir;
    for (int t = 0; t < 3; ++t) {
        s.tracks[t].inUse = true;
        s.tracks[t].volume = t == 0 ? 0.05f : t == 1 ? 1.0f / 3.0f : 0.123456789f;
    }
    FakeCard card;
    const uint64_t before = element_content_hash(s, card.reader());
    REQUIRE(s.save());
    AudioSong back;
    REQUIRE(back.load(dir) == SongLoad::Loaded);
    back.dir = dir;
    CHECK(element_content_hash(back, card.reader()) == before);
    std::filesystem::remove_all(dir);
}

TEST_CASE("element hash: a kit named from the card's root is read where the card is mounted", "[element][hash]") {
    struct Mount {
        Mount() { setKitPathFsPrefix("/sdcard"); }
        ~Mount() { setKitPathFsPrefix(""); }
    } mount;
    FakeCard card = baseCard();
    card.files["/sdcard/crosspad/kits/K/kit.json"] = "{}";
    card.files["/sdcard/crosspad/kits/K/kick.wav"] = "kick";
    AudioSong s = baseSong();
    s.kit = "/crosspad/kits/K/kit.json";
    const uint64_t h = hashOf(s, card);
    FakeCard edited = card;
    edited.files["/sdcard/crosspad/kits/K/kit.json"] = "{ }";
    CHECK(hashOf(s, edited) != h);
    FakeCard swapped = card;
    swapped.files["/sdcard/crosspad/kits/K/kick.wav"] = "kicks";
    CHECK(hashOf(s, swapped) != h);
}

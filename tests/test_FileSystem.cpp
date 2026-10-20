// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "fake_file_system.hpp"

#include "crosspad/kit/KitPathUtils.hpp"
#include "crosspad/kit/PortableKitLoader.hpp"
#include "crosspad/platform/PosixFileSystem.hpp"
#include "crosspad/sequencer/PatternSequencer.hpp"
#include "crosspad/sequencer/SequenceStore.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace crosspad;

namespace {

/// A directory of its own under the temp dir, gone again afterwards.
struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("fstest_" + std::to_string(std::random_device{}()) + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
    std::string str(const std::string& under = "") const { return under.empty() ? path.string() : (path / under).string(); }
};

void touch(const std::string& path, const std::string& content = "")
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f);
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
}

std::vector<std::pair<std::string, bool>> listed(IFileSystem& fsys, const std::string& dir)
{
    std::vector<std::pair<std::string, bool>> out;
    fsys.forEach(dir, [&](const char* name, bool isDir) { out.emplace_back(name, isDir); });
    std::sort(out.begin(), out.end());
    return out;
}

size_t openDescriptors()
{
    size_t n = 0;
    for (auto it = fs::directory_iterator("/proc/self/fd"); it != fs::directory_iterator(); ++it) ++n;
    return n;
}

} // namespace

TEST_CASE("posix fs: a listing names every entry but . and .., with its type", "[fs]") {
    TempDir t;
    PosixFileSystem p;
    REQUIRE(p.makeDir(t.str("sub").c_str()));
    REQUIRE(p.makeDir(t.str(".fx").c_str()));
    touch(t.str("a.wav"));
    using E = std::pair<std::string, bool>;
    CHECK(listed(p, t.str()) == std::vector<E>{{".fx", true}, {"a.wav", false}, {"sub", true}});
    CHECK_FALSE(p.forEach(t.str("nope"), [](const char*, bool) {}));
}

TEST_CASE("posix fs: a callback that answers false stops the listing", "[fs]") {
    TempDir t;
    PosixFileSystem p;
    for (int i = 0; i < 5; ++i) touch(t.str("f" + std::to_string(i)));
    int seen = 0;
    CHECK(p.forEach(t.str(), [&](const char*, bool) { return ++seen < 2; }));
    CHECK(seen == 2);
}

TEST_CASE("posix fs: a symlink to a folder lists as a folder", "[fs]") {
    TempDir t;
    PosixFileSystem p;
    REQUIRE(p.makeDir(t.str("real").c_str()));
    fs::create_directory_symlink(t.path / "real", t.path / "link");
    using E = std::pair<std::string, bool>;
    CHECK(listed(p, t.str()) == std::vector<E>{{"link", true}, {"real", true}});
}

TEST_CASE("posix fs: stat tells missing from there, and gives size and type", "[fs]") {
    TempDir t;
    PosixFileSystem p;
    touch(t.str("f.bin"), "12345");
    FileStat st;
    REQUIRE(p.stat(t.str("f.bin").c_str(), &st) == FsStat::Found);
    CHECK(st.size == 5);
    CHECK_FALSE(st.isDir);
    CHECK(st.modified > 0);
    CHECK(p.stat(t.str("gone").c_str(), nullptr) == FsStat::Missing);
    CHECK(p.isDir(t.str().c_str()));
    CHECK(p.exists(t.str("f.bin").c_str()));
    CHECK_FALSE(p.isDir(t.str("f.bin").c_str()));
}

TEST_CASE("posix fs: makeDir is true for a folder already there, false for a file", "[fs]") {
    TempDir t;
    PosixFileSystem p;
    CHECK(p.makeDir(t.str("d").c_str()));
    CHECK(p.makeDir(t.str("d").c_str()));
    touch(t.str("f"));
    CHECK_FALSE(p.makeDir(t.str("f").c_str()));
    CHECK_FALSE(p.makeDir(t.str("no/such/parent").c_str()));
}

TEST_CASE("posix fs: rename, remove and removeDir", "[fs]") {
    TempDir t;
    PosixFileSystem p;
    touch(t.str("a"));
    CHECK(p.rename(t.str("a").c_str(), t.str("b").c_str()));
    CHECK_FALSE(p.exists(t.str("a").c_str()));
    CHECK(p.remove(t.str("b").c_str()));
    CHECK_FALSE(p.remove(t.str("b").c_str()));
    REQUIRE(p.makeDir(t.str("d").c_str()));
    touch(t.str("d/x"));
    CHECK_FALSE(p.removeDir(t.str("d").c_str()));   // not empty
    CHECK(p.remove(t.str("d/x").c_str()));
    CHECK(p.removeDir(t.str("d").c_str()));
}

TEST_CASE("posix fs: removeTree takes hidden folders too, and reports every entry", "[fs]") {
    TempDir t;
    PosixFileSystem p;
    const std::string kit = t.str("KIT");
    REQUIRE(p.makeDir(kit.c_str()));
    REQUIRE(p.makeDir((kit + "/.fx").c_str()));
    touch(kit + "/.fx/1_p+3.wav");
    touch(kit + "/kit.json");
    int entries = 0;
    CHECK(p.removeTree(kit, [](void* n) { ++*static_cast<int*>(n); }, &entries));
    CHECK(entries == 3);
    CHECK_FALSE(fs::exists(kit));
    CHECK_FALSE(p.removeTree(kit));
}

TEST_CASE("posix fs: unbuffered reads seek and stop at the end", "[fs]") {
    TempDir t;
    PosixFileSystem p;
    touch(t.str("r"), "abcdef");
    const int fd = p.openRead(t.str("r").c_str());
    REQUIRE(fd >= 0);
    char buf[8] = {};
    CHECK(p.read(fd, buf, 2) == 2);
    CHECK(std::string(buf, 2) == "ab");
    CHECK(p.seek(fd, 4));
    CHECK(p.read(fd, buf, sizeof buf) == 2);
    CHECK(std::string(buf, 2) == "ef");
    CHECK(p.read(fd, buf, sizeof buf) == 0);
    p.close(fd);
    CHECK(p.openRead(t.str("none").c_str()) < 0);
}

TEST_CASE("posix fs: a listing whose callback throws leaves no directory open", "[fs]") {
    if (!fs::exists("/proc/self/fd")) SKIP("no /proc/self/fd here");
    TempDir t;
    PosixFileSystem p;
    touch(t.str("a"));
    const size_t before = openDescriptors();
    CHECK_THROWS_AS(p.forEach(t.str(), [](const char*, bool) { throw std::bad_alloc(); }), std::bad_alloc);
    CHECK(openDescriptors() == before);
}

TEST_CASE("file system: the platform's is the POSIX one until another is set", "[fs]") {
    test::FakeFileSystem fake;
    IFileSystem* const before = &getFileSystem();
    CHECK(dynamic_cast<PosixFileSystem*>(before) != nullptr);
    {
        test::UseFileSystem use(fake);
        CHECK(&getFileSystem() == &fake);
    }
    CHECK(&getFileSystem() == before);
}

// ── Portable code, against a card that is not there ─────────────────────────

TEST_CASE("sequence store: a file that will not open is not 'no scenes'", "[fs][sequencer]") {
    test::FakeFileSystem fake;
    test::UseFileSystem use(fake);
    PatternSequencer seq;
    /* The file system knows it, stdio cannot open it: on the board, every FAT
     * handle taken at boot. */
    const std::string path = "/fstest-no-such-mount/sequences.json";
    fake.addFile(path, 1200);
    CHECK(SequenceStore::open(seq, path) == SequenceLoad::Unreadable);
}

TEST_CASE("sequence store: absent only when none of the three is there", "[fs][sequencer]") {
    test::FakeFileSystem fake;
    test::UseFileSystem use(fake);
    PatternSequencer seq;
    const std::string path = "/fstest-no-such-mount/sequences.json";
    CHECK(SequenceStore::open(seq, path) == SequenceLoad::Absent);
    fake.failStat(path + ".bak");
    CHECK(SequenceStore::open(seq, path) == SequenceLoad::Unreadable);
}

TEST_CASE("kit scan: files cost no stat, hidden folders are not looked into", "[fs][kit]") {
    test::FakeFileSystem fake;
    test::UseFileSystem use(fake);
    fake.addDir("/card");
    fake.addDir("/card/kits");
    for (int i = 0; i < 5; ++i) fake.addFile("/card/kits/loose" + std::to_string(i) + ".wav");
    fake.addDir("/card/kits/.trash");
    fake.addDir("/card/kits/GROUP");
    fake.addDir("/card/kits/GROUP/EMPTY");
    PortableKitLoader loader("/card");
    loader.scanKits("/kits");
    CHECK(loader.kits().empty());
    /* Per folder: kit.json, <name>.json and the .bak of each. */
    CHECK(fake.stats == 2 * 4);
    CHECK(fake.lists == 3);   // kits, GROUP, GROUP/EMPTY
    setKitPathFsPrefix("");
}

TEST_CASE("kit removal: the folder goes, with the copies rendered for its pads", "[fs][kit]") {
    test::FakeFileSystem fake;
    test::UseFileSystem use(fake);
    fake.addDir("/card");
    fake.addDir("/card/kits");
    fake.addDir("/card/kits/A");
    fake.addFile("/card/kits/A/kit.json");
    fake.addDir("/card/kits/A/.fx");
    fake.addFile("/card/kits/A/.fx/1_r.wav");
    fake.addDir("/card/kits/B");
    PortableKitLoader loader("/card");
    KitInfo a, b;
    a.path = "/kits/A/kit.json";
    b.path = "/kits/B/kit.json";
    loader.kits().push_back(a);
    loader.kits().push_back(b);
    REQUIRE(loader.removeKit(0));
    CHECK_FALSE(fake.has("/card/kits/A"));
    CHECK_FALSE(fake.has("/card/kits/A/.fx/1_r.wav"));
    CHECK(fake.has("/card/kits/B"));
    CHECK(loader.kits()[0].path.empty());
    setKitPathFsPrefix("");
}

TEST_CASE("kit paths: a sample is looked for flat, then under SAMPLES", "[fs][kit]") {
    test::FakeFileSystem fake;
    test::UseFileSystem use(fake);
    setKitPathFsPrefix("/card");
    fake.addFile("/card/kits/K/SAMPLES/kick.wav");
    CHECK(resolveKitSamplePath("/kits/K/kit.json", "kick.wav") == "/kits/K/SAMPLES/kick.wav");
    fake.addFile("/card/kits/K/kick.wav");
    CHECK(resolveKitSamplePath("/kits/K/kit.json", "kick.wav") == "/kits/K/kick.wav");
    CHECK(resolveKitSamplePath("/kits/K/kit.json", "snare.wav") == "/kits/K/snare.wav");
    setKitPathFsPrefix("");
}

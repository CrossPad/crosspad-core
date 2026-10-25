// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "crosspad/song/SongFiles.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace crosspad;
namespace fs = std::filesystem;

namespace {
std::string tempRoot(const char* name)
{
    const fs::path p = fs::temp_directory_path() / "cp_core_tests" / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p.string();
}
void put(const std::string& path, const std::string& text)
{
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << text;
}
std::string get(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE("song files: a three-piece file is copied byte for byte", "[song][files]") {
    const auto d = tempRoot("files_copy");
    std::string body;
    for (int i = 0; i < 4096 * 2 + 100; ++i) body += char(i * 7);
    put(d + "/a.bin", body);
    int yields = 0;
    REQUIRE(song_copy_file(d + "/a.bin", d + "/b.bin", [&] { ++yields; }));
    CHECK(get(d + "/b.bin") == body);
    CHECK(yields == 3);
    CHECK_FALSE(fs::exists(d + "/b.bin.part"));
    put(d + "/b.bin", "old");
    REQUIRE(song_copy_file(d + "/a.bin", d + "/b.bin", nullptr));
    CHECK(get(d + "/b.bin") == body);
    fs::remove_all(d);
}

TEST_CASE("song files: a failed copy leaves no file", "[song][files]") {
    const auto d = tempRoot("files_fail");
    put(d + "/a.bin", "data");
    fs::create_directories(d + "/dir");
    CHECK_FALSE(song_copy_file(d + "/a.bin", d + "/dir", nullptr));
    CHECK(fs::is_directory(d + "/dir"));
    CHECK_FALSE(fs::exists(d + "/dir.part"));
    CHECK_FALSE(song_copy_file(d + "/a.bin", d + "/missing/b.bin", nullptr));
    CHECK_FALSE(song_copy_file(d + "/absent.bin", d + "/c.bin", nullptr));
    CHECK_FALSE(fs::exists(d + "/c.bin"));
    CHECK_FALSE(fs::exists(d + "/c.bin.part"));
    fs::remove_all(d);
}

TEST_CASE("song files: a tree copy skips what keep refuses", "[song][files]") {
    const auto d = tempRoot("files_tree");
    put(d + "/src/song.json", "{}");
    put(d + "/src/clips/a.wav", "wav");
    put(d + "/src/clips/.freeze/f.wav", "frozen");
    put(d + "/src/.peaks/p", "peaks");
    const auto keep = [](const std::string& rel) { return rel != "clips/.freeze" && rel != ".peaks"; };
    REQUIRE(song_copy_tree(d + "/src", d + "/dst", keep, nullptr));
    CHECK(get(d + "/dst/song.json") == "{}");
    CHECK(get(d + "/dst/clips/a.wav") == "wav");
    CHECK_FALSE(fs::exists(d + "/dst/clips/.freeze"));
    CHECK_FALSE(fs::exists(d + "/dst/.peaks"));
    CHECK_FALSE(song_copy_tree(d + "/nothing", d + "/dst2", keep, nullptr));
    fs::remove_all(d);
}

TEST_CASE("song files: folders listed sorted, files left out", "[song][files]") {
    const auto d = tempRoot("files_list");
    fs::create_directories(d + "/E00000002");
    fs::create_directories(d + "/E00000001");
    put(d + "/note.txt", "x");
    const auto names = song_list_dirs(d);
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "E00000001");
    CHECK(names[1] == "E00000002");
    CHECK(song_list_dirs(d + "/none").empty());
    fs::remove_all(d);
}

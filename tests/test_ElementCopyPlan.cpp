// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "crosspad/song/ElementCopyPlan.hpp"

using namespace crosspad;

namespace {

/// Who plays what, written as "user > element" pairs; a user in capitals is a song.
ElementUses usesOf(std::initializer_list<std::pair<const char*, const char*>> plays) {
    ElementUses uses;
    for (const auto& p : plays) {
        const std::string user = p.first;
        const bool song = !user.empty() && user[0] >= 'A' && user[0] <= 'Z';
        uses[p.second].push_back({user, song});
    }
    return uses;
}

using Refs = std::vector<std::string>;

} // namespace

TEST_CASE("copy plan: a shared element under one heard only in the song re-points that one", "[element][copyplan]") {
    /* SONG > a > b; OTHER plays b too. */
    const auto uses = usesOf({{"SONG", "a"}, {"a", "b"}, {"OTHER", "b"}});
    const ElementCopyPlan p = plan_element_copies("SONG", uses, {"b"}, {});
    CHECK(p.forks == Refs{"b"});
    CHECK(p.rewrites == Refs{"a"});
    CHECK_FALSE(p.song);
}

TEST_CASE("copy plan: the copies go up the path while the parent is shared", "[element][copyplan]") {
    /* SONG > a > b; OTHER plays a, OTHER2 plays b. */
    const auto uses = usesOf({{"SONG", "a"}, {"OTHER", "a"}, {"a", "b"}, {"OTHER2", "b"}});
    const ElementCopyPlan p = plan_element_copies("SONG", uses, {"b"}, {});
    CHECK(p.forks == Refs{"b", "a"});
    CHECK(p.rewrites.empty());
    CHECK(p.song);
}

TEST_CASE("copy plan: an element heard only under a shared one is shared too", "[element][copyplan]") {
    /* SONG > a > y > b; OTHER plays a: y is heard there through a. */
    const auto uses = usesOf({{"SONG", "a"}, {"OTHER", "a"}, {"a", "y"}, {"y", "b"}, {"z", "b"}});
    const ElementCopyPlan p = plan_element_copies("SONG", uses, {"b"}, {});
    CHECK(p.forks == Refs{"b", "y", "a"});
    CHECK(p.rewrites.empty());
    CHECK(p.song);
}

TEST_CASE("copy plan: every occurrence in the song gets the copy, a shared holder copied", "[element][copyplan]") {
    /* SONG > a > b, SONG > v > b, SONG > w > b; OTHER plays b, OTHER3 plays w. */
    const auto uses = usesOf({{"SONG", "a"}, {"SONG", "v"}, {"SONG", "w"}, {"OTHER3", "w"},
                              {"a", "b"}, {"v", "b"}, {"w", "b"}, {"OTHER", "b"}});
    const ElementCopyPlan p = plan_element_copies("SONG", uses, {"b"}, {});
    CHECK(p.forks == Refs{"b", "w"});
    CHECK(p.rewrites == Refs{"a", "v"});
    CHECK(p.song);
}

TEST_CASE("copy plan: an element outside the song keeps the original", "[element][copyplan]") {
    /* lib plays b and is in no song; SONG > a > b. */
    const auto uses = usesOf({{"SONG", "a"}, {"a", "b"}, {"lib", "b"}});
    const ElementCopyPlan p = plan_element_copies("SONG", uses, {"b"}, {});
    CHECK(p.forks == Refs{"b"});
    CHECK(p.rewrites == Refs{"a"});
}

TEST_CASE("copy plan: back at the song, the rest of it found from the copies made", "[element][copyplan]") {
    /* On the way up b became b2 and a was re-pointed to it; v still plays b. */
    const auto uses = usesOf({{"SONG", "a"}, {"SONG", "v"}, {"a", "b2"}, {"v", "b"}, {"OTHER", "b"}});
    const ElementCopyPlan p = plan_element_copies("SONG", uses, {}, {"b"});
    CHECK(p.forks.empty());
    CHECK(p.rewrites == Refs{"v"});
    CHECK_FALSE(p.song);
}

TEST_CASE("copy plan: an original copied keeps what it plays, and nothing is copied twice", "[element][copyplan]") {
    /* On the way up c became c2 inside b, and b (shared) became b2; SONG plays c directly too. */
    const auto uses = usesOf({{"SONG", "b2"}, {"SONG", "c"}, {"OTHER", "b"}, {"b", "c"}, {"b2", "c2"}, {"z", "c"}});
    const ElementCopyPlan p = plan_element_copies("SONG", uses, {}, {"c", "b"});
    CHECK(p.forks.empty());
    CHECK(p.rewrites.empty());
    CHECK(p.song);
    /* Asked for again, an element copied before is copied again: what plays it is found once. */
    const ElementCopyPlan again = plan_element_copies("SONG", uses, {"c", "c"}, {"c"});
    CHECK(again.forks == Refs{"c"});
    CHECK(again.song);
}

TEST_CASE("copy plan: a nest that goes round ends", "[element][copyplan]") {
    const auto uses = usesOf({{"SONG", "a"}, {"a", "b"}, {"b", "a"}, {"OTHER", "b"}});
    const ElementCopyPlan p = plan_element_copies("SONG", uses, {"b"}, {});
    CHECK(p.forks == Refs{"b", "a"});
    CHECK(p.song);
}

TEST_CASE("copy plan: a copy's title is the next number free", "[element][copyplan]") {
    CHECK(element_copy_title("REFREN", {}) == "REFREN 2");
    CHECK(element_copy_title("REFREN 2", {"REFREN", "REFREN 2"}) == "REFREN 3");
    CHECK(element_copy_title("LOOP DR", {"LOOP DR 2", "LOOP DR 3"}) == "LOOP DR 4");
    CHECK(element_copy_title("", {}) == "ELEMENT 2");
    CHECK(element_copy_title("TAKE 1234567", {}) == "TAKE 1234567 2");
}

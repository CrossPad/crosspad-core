// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "crosspad/kit/KitInfo.hpp"

#include <utility>
#include <vector>

using crosspad::KitInfo;
using crosspad::Pad;

TEST_CASE("KitPads: a listed kit costs a pointer, not its pads", "[kit][pads]") {
    STATIC_REQUIRE(sizeof(KitInfo) < sizeof(Pad) * KIT_PADS);
    KitInfo k;
    REQUIRE_FALSE(k.pads.allocated());
    const KitInfo& c = k;
    CHECK(c.pads[3].samples.empty());          // read as a default pad
    int n = 0;
    for (const Pad& p : c.pads) { CHECK(p.samples.empty()); ++n; }
    CHECK(n == KIT_PADS);
    CHECK_FALSE(k.pads.allocated());           // reading never allocates
}

TEST_CASE("KitPads: a pad written to is the kit's own", "[kit][pads]") {
    KitInfo k;
    k.pads[5].AddSample("hit.wav", false);
    REQUIRE(k.pads.allocated());
    CHECK(k.pads[5].samples.size() == 1);
    CHECK(k.pads[4].samples.empty());

    KitInfo copy = k;                          // a copy carries them
    REQUIRE(copy.pads.allocated());
    CHECK(copy.pads[5].samples.size() == 1);
    CHECK(copy.pads.data() != k.pads.data());
}

TEST_CASE("KitPads: the block stays where it is for the kit's life", "[kit][pads]") {
    KitInfo k;
    Pad* block = k.pads;                       // what PadManager::loadPadConfig() keeps
    REQUIRE(block != nullptr);

    KitInfo loaded;
    loaded.pads[0].AddSample("a.wav", false);
    k = loaded;                                // the loader's kits_[id] = kit
    CHECK(k.pads.data() == block);
    CHECK(block[0].samples.size() == 1);

    KitInfo keep;                              // held while k is reassigned, so its block
    keep.pads[1].AddSample("b.wav", false);    // cannot be handed back at the same address
    k = KitInfo{};                             // the loader blanking a deleted kit
    CHECK(k.pads.data() == block);
    CHECK(block[0].samples.empty());
    k = std::move(keep);                       // moved into: the contents move, not the block
    CHECK(k.pads.data() == block);
    CHECK(block[1].samples.size() == 1);

    std::vector<KitInfo> list;
    list.push_back(std::move(k));              // and moved, as a growing list does
    list.reserve(list.capacity() + 8);
    CHECK(list[0].pads.data() == block);
}

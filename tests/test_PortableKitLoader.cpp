// SPDX-License-Identifier: GPL-3.0-or-later
//
// The kit list's room: every kit on the card listed with kSpareKits to spare,
// kits made while the board runs added into it, and a removed kit's slot
// taken once the spare is gone.
#include <catch2/catch_test_macros.hpp>
#include "crosspad/kit/PortableKitLoader.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using crosspad::PortableKitLoader;
namespace fs = std::filesystem;

namespace {

constexpr const char* kKits = "/crosspad/kits";

/// A card root of its own, with kit folders written under crosspad/kits.
struct Card {
    explicit Card(const char* name) : root(name)
    {
        fs::remove_all(root);
        fs::create_directories(root + kKits);
    }
    ~Card() { fs::remove_all(root); }

    /// One kit folder with its kit.json; the logical path addKit() takes.
    std::string kit(const std::string& name) const
    {
        const std::string dir = root + kKits + "/" + name;
        fs::create_directories(dir);
        std::ofstream(dir + "/kit.json") << "{\"name\":\"" << name << "\",\"pads\":[]}";
        return std::string(kKits) + "/" + name + "/kit.json";
    }

    std::string root;
};

} // namespace

TEST_CASE("scanKits lists every kit and leaves kSpareKits of room", "[kit]")
{
    Card card("kitroom_scan");
    constexpr int kOnCard = 40;   // past the old first guess of 32, which the list doubled out of
    for (int i = 0; i < kOnCard; ++i) card.kit("K" + std::to_string(i));
    PortableKitLoader loader(card.root);
    loader.scanKits(kKits);
    REQUIRE(loader.getKitCount() == kOnCard);

    for (size_t i = 0; i < PortableKitLoader::kSpareKits; ++i) {
        const int id = loader.addKit(card.kit("NEW" + std::to_string(i)));
        CHECK(id == kOnCard + static_cast<int>(i));
    }
    CHECK(loader.getKitCount() == kOnCard + static_cast<int>(PortableKitLoader::kSpareKits));
}

TEST_CASE("addKit takes a removed kit's slot once the spare is used up", "[kit]")
{
    Card card("kitroom_reuse");
    for (int i = 0; i < 3; ++i) card.kit("K" + std::to_string(i));
    PortableKitLoader loader(card.root);
    loader.scanKits(kKits);
    REQUIRE(loader.getKitCount() == 3);
    for (size_t i = 0; i < PortableKitLoader::kSpareKits; ++i) {
        REQUIRE(loader.addKit(card.kit("NEW" + std::to_string(i))) >= 0);
    }

    const std::string late = card.kit("LATE");
    CHECK(loader.addKit(late) == -1);   // full, and nothing removed yet

    REQUIRE(loader.removeKit(1));
    CHECK(loader.getKit(1)->path.empty());
    CHECK(loader.addKit(late) == 1);
    CHECK(loader.getKit(1)->name == "LATE");
    CHECK(loader.findKitIdByPath(late) == 1);
}

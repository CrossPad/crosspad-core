#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include "crosspad/fw/FwStore.hpp"

using namespace crosspad;

namespace {

// The same entry the catalog parser accepts: names, sizes, tag, manifest, sig.
std::string sver(const std::string& version, const char* channel, const char* tier = "official",
                 const std::string& extra = "", uint32_t bin = 2000000, uint32_t assets = 1000000) {
    const std::string base = "CrossPad-v" + version + "-v2-k";
    return "{\"version\":\"" + version + "\",\"tag\":\"v" + version + "\",\"channel\":\"" + channel +
           "\",\"tier\":\"" + tier + "\",\"bin\":{\"name\":\"" + base + ".bin\",\"size\":" + std::to_string(bin) +
           "},\"assets\":{\"name\":\"" + base + "-assets.bin\",\"size\":" + std::to_string(assets) +
           "},\"manifest\":\"" + base + "-manifest.json\",\"sig\":\"" + base + "-manifest.sig\"" + extra + "}";
}

std::string skit(const std::string& kit, const std::string& versions, const std::string& kitMeta = "") {
    return "{\"kit\":\"" + kit + "\"" + kitMeta + ",\"versions\":[" + versions + "]}";
}

std::string envelope(const std::string& kits, const char* rev = "v2", int v = 1) {
    return "{\"v\":" + std::to_string(v) + ",\"fetched\":1790964000,\"rev\":\"" + rev +
           "\",\"catalog\":{\"v\":1,\"generated\":\"x\",\"kits\":[" + kits + "]}}";
}

std::unique_ptr<FwStore> load(const std::string& json, FwStoreLoad want = FwStoreLoad::Ok) {
    auto s = std::make_unique<FwStore>();
    REQUIRE(parseFwStoreCache(json.data(), json.size(), 2, *s) == want);
    return s;
}

// The catalog names a kit per kit and its icon per version (decision 3).
const std::string kKitMeta = ",\"display_name\":\"Drum Machine\",\"author\":\"Ola\"";
const std::string kIcon = ",\"icon\":{\"name\":\"CrossPad-v1.3.0-v2-drums-icon.bin\",\"size\":6924}";
// What /api/device/kit/<kit> adds to a version, as .details/<kit>.json keeps it.
const std::string kDetail =
    ",\"display_name\":\"Drum Machine\",\"author\":\"Ola\",\"description\":\"Pads and a sequencer.\","
    "\"apps\":[\"sampler\",\"pad-mixer\"],\"notes\":\"Faster kit loads\\nFixed the mixer\"";

const std::string kTwo = envelope(
    skit("full", sver("1.4.0-rc1", "beta") + "," + sver("1.3.0", "stable") + "," + sver("1.2.0", "stable")) + "," +
    skit("drums", sver("1.3.0", "stable", "community", kIcon) + "," + sver("1.1.0", "stable", "community"), kKitMeta) +
    "," + skit("alpha", sver("0.9.0", "beta")));

}  // namespace

TEST_CASE("store cache parses kits, sizes, icons and display fields", "[fw]") {
    auto s = load(kTwo);
    CHECK(s->fetched == 1790964000);
    REQUIRE(s->kitCount == 3);
    const FwStoreKit* drums = fwStoreFind(*s, "drums");
    REQUIRE(drums);
    CHECK(std::strcmp(drums->displayName, "Drum Machine") == 0);
    CHECK(std::strcmp(drums->author, "Ola") == 0);
    CHECK(drums->tier == FwTier::Community);
    REQUIRE(drums->count == 2);
    CHECK(drums->versions[0].hasIcon);
    CHECK(drums->versions[0].bytes == 2000000u + 1000000u + 6924u);
    CHECK_FALSE(drums->versions[1].hasIcon);
    const FwStoreKit* full = fwStoreFind(*s, "full");
    REQUIRE(full);
    CHECK(std::strcmp(full->displayName, "full") == 0);   // no display_name: the kit id
    CHECK(fwStoreVersionIndex(*full, "v1.3.0") == 1);
    CHECK(fwStoreVersionIndex(*full, "9.9.9") == -1);
}

TEST_CASE("store cache refuses another envelope, another board, broken JSON", "[fw]") {
    load("", FwStoreLoad::Format);
    load("{\"v\":1,\"fetched\":1,\"rev\":\"v2\",\"catalog\":", FwStoreLoad::Format);   // a card pulled mid-write
    load(envelope(skit("full", sver("1.3.0", "stable")), "v2", 2), FwStoreLoad::Format);
    load(envelope(skit("full", sver("1.3.0", "stable")), "v1"), FwStoreLoad::OtherBoard);
    load(envelope(skit("full", sver("1.3.0", "stable")), "two"), FwStoreLoad::Format);
    auto s = load(envelope(skit("Bad Kit", sver("1.0.0", "stable")) + "," + skit("empty", sver("one", "stable"))));
    CHECK(s->kitCount == 0);                                // nothing usable is still a readable cache
    CHECK(std::strcmp(fwStoreLoadName(FwStoreLoad::OtherBoard), "other_board") == 0);
}

TEST_CASE("the shown version follows the channel and falls back to beta", "[fw]") {
    auto s = load(kTwo);
    const FwStoreKit* full = fwStoreFind(*s, "full");
    CHECK(fwShownVersion(*full, FwChannel::Stable) == 1);   // 1.3.0
    CHECK(fwShownVersion(*full, FwChannel::Beta) == 0);     // 1.4.0-rc1
    const FwStoreKit* alpha = fwStoreFind(*s, "alpha");
    CHECK(fwShownVersion(*alpha, FwChannel::Stable) == 0);  // a beta-only kit is still shown
}

TEST_CASE("store order: official first, then community, each by name", "[fw]") {
    auto s = load(envelope(skit("zeta", sver("1.0.0", "stable")) + "," +
                           skit("drums", sver("1.0.0", "stable", "community"), kKitMeta) + "," +
                           skit("beta", sver("1.0.0", "stable")) + "," +
                           skit("bass", sver("1.0.0", "stable", "community"))));
    uint8_t order[kFwCatalogKits];
    size_t firstCommunity = 0;
    REQUIRE(fwStoreOrder(*s, order, kFwCatalogKits, &firstCommunity) == 4);
    CHECK(firstCommunity == 2);
    CHECK(std::strcmp(s->kits[order[0]].kit, "beta") == 0);
    CHECK(std::strcmp(s->kits[order[1]].kit, "zeta") == 0);
    CHECK(std::strcmp(s->kits[order[2]].kit, "bass") == 0);
    CHECK(std::strcmp(s->kits[order[3]].kit, "drums") == 0);   // "Drum Machine" sorts after "bass"
}

TEST_CASE("older versions: everything below the newest, newest first", "[fw]") {
    auto s = load(envelope(skit("full", sver("1.2.0", "stable") + "," + sver("1.4.0-rc1", "beta") + "," +
                                        sver("1.3.0", "stable") + "," + sver("1.0.0", "stable"))));
    uint8_t older[kFwCatalogVersions];
    const FwStoreKit& k = s->kits[0];
    REQUIRE(fwStoreOlder(k, older, kFwCatalogVersions) == 3);
    CHECK(std::strcmp(k.versions[older[0]].version, "1.3.0") == 0);
    CHECK(std::strcmp(k.versions[older[1]].version, "1.2.0") == 0);
    CHECK(std::strcmp(k.versions[older[2]].version, "1.0.0") == 0);
}

TEST_CASE("update rows: newer in the kit's channel than what the card holds", "[fw]") {
    auto s = load(kTwo);
    const FwOwned owned[] = {{"full", "v1.2.0"}, {"full", "1.3.0"}, {"drums", "1.1.0"}, {"lite", "1.0.0"},
                             {"alpha", "v0.9.0-12-gabcdef0"}};
    FwUpdateRow rows[8];
    auto stable = [](const char*, void*) { return false; };
    size_t n = fwStoreUpdates(*s, owned, 5, stable, nullptr, rows, 8);
    REQUIRE(n == 1);                                         // full has 1.3.0 already; alpha only has beta
    CHECK(std::strcmp(s->kits[rows[0].kit].kit, "drums") == 0);
    CHECK(std::strcmp(s->kits[rows[0].kit].versions[rows[0].version].version, "1.3.0") == 0);
    CHECK(std::strcmp(rows[0].have, "1.1.0") == 0);
    auto fullBeta = [](const char* kit, void*) { return std::strcmp(kit, "full") == 0; };
    n = fwStoreUpdates(*s, owned, 5, fullBeta, nullptr, rows, 8);
    REQUIRE(n == 2);
    CHECK(std::strcmp(s->kits[rows[0].kit].kit, "full") == 0);   // official first
    CHECK(std::strcmp(s->kits[rows[0].kit].versions[rows[0].version].version, "1.4.0-rc1") == 0);
    CHECK(std::strcmp(rows[0].have, "1.3.0") == 0);
}

TEST_CASE("kit card text from a details file and from a manifest", "[fw]") {
    const std::string kitObj = skit("drums", sver("1.3.0", "stable", "community", kDetail) + "," +
                                             sver("1.1.0", "stable", "community", ",\"notes\":\"Old notes\""));
    FwKitDetail d{};
    REQUIRE(parseFwKitDetail(kitObj.data(), kitObj.size(), "v1.3.0", d));
    CHECK(std::strcmp(d.displayName, "Drum Machine") == 0);
    CHECK(std::strcmp(d.description, "Pads and a sequencer.") == 0);
    CHECK(std::strcmp(d.notes, "Faster kit loads\nFixed the mixer") == 0);
    REQUIRE(d.appCount == 2);
    CHECK(std::strcmp(d.apps[1], "pad-mixer") == 0);
    CHECK(d.tier == FwTier::Community);
    REQUIRE(parseFwKitDetail(kitObj.data(), kitObj.size(), "1.1.0", d));
    CHECK(std::strcmp(d.notes, "Old notes") == 0);
    CHECK(d.displayName[0] == '\0');
    CHECK_FALSE(parseFwKitDetail(kitObj.data(), kitObj.size(), "2.0.0", d));

    const char* manifest = R"({"name":"drums-1.3.0","kit":"drums","version":"1.3.0","board_rev":2,
        "tier":"community","display_name":"Drum Machine","author":"Ola","apps":["sampler"],"files":{}})";
    FwKitDetail m{};
    REQUIRE(parseFwManifestDetail(manifest, std::strlen(manifest), m));   // display only: no files needed
    CHECK(std::strcmp(m.author, "Ola") == 0);
    CHECK(m.notes[0] == '\0');
    fwKitDetailMerge(m, d);                                    // fills only what the manifest left empty
    CHECK(std::strcmp(m.displayName, "Drum Machine") == 0);
    CHECK(std::strcmp(m.notes, "Old notes") == 0);
    CHECK(m.appCount == 1);
}

TEST_CASE("fwCopyText never splits a UTF-8 sequence", "[fw]") {
    char out[6];
    fwCopyText(out, sizeof out, "abcd\xC5\x82xyz");   // "abcdł..." : the 2-byte letter would straddle the cut
    CHECK(std::strcmp(out, "abcd") == 0);
    fwCopyText(out, sizeof out, "ab\xE2\x82\xAC" "c");  // "ab€c": exactly 5 bytes fit
    CHECK(std::strcmp(out, "ab\xE2\x82\xAC") == 0);
    fwCopyText(out, sizeof out, nullptr);
    CHECK(out[0] == '\0');
    char big[kFwDisplayNameMax];
    std::string name(30, 'x');
    name += "\xC5\x9A";                                 // 32 bytes: the trailing "Ś" does not fit in 31
    fwCopyText(big, sizeof big, name.c_str());
    CHECK(std::strlen(big) == 30);
}

TEST_CASE("store cache keeps the first of two equal versions and drops an unsafe icon name", "[fw]") {
    const std::string badIcon = ",\"icon\":{\"name\":\"../icon.bin\",\"size\":6924}";
    auto s = load(envelope(skit("full", sver("1.3.0", "stable", "official", badIcon) + "," + sver("1.3.0", "beta"))));
    REQUIRE(s->kitCount == 1);
    REQUIRE(s->kits[0].count == 1);
    CHECK(s->kits[0].versions[0].channel == FwChannel::Stable);
    CHECK_FALSE(s->kits[0].versions[0].hasIcon);
    CHECK(s->kits[0].versions[0].bytes == 3000000u);
}

TEST_CASE("kit detail truncates display text at a character and drops unsafe app ids", "[fw]") {
    std::string description(kFwDescriptionMax - 2, 'd');
    description += "\xC5\x82xyz";   // the 2-byte letter would straddle the cut at 191 bytes
    const std::string extra = ",\"description\":\"" + description +
                              "\",\"apps\":[\"sampler\",\"../x\",\"has space\",\"\",\"pad-mixer\"]";
    const std::string kitObj = skit("drums", sver("1.3.0", "stable", "official", extra));
    FwKitDetail d{};
    REQUIRE(parseFwKitDetail(kitObj.data(), kitObj.size(), "1.3.0", d));
    CHECK(std::strlen(d.description) == kFwDescriptionMax - 2);
    REQUIRE(d.appCount == 2);
    CHECK(std::strcmp(d.apps[0], "sampler") == 0);
    CHECK(std::strcmp(d.apps[1], "pad-mixer") == 0);
}

TEST_CASE("store cache applies the catalog's version rules", "[fw]") {
    auto s = load(envelope(skit("full",
        sver("1.0.0", "stable") + "," +
        "{\"version\":\"1.1.0\",\"tag\":\"v1.1.0\",\"channel\":\"stable\",\"tier\":\"official\","
        "\"bin\":{\"size\":1},\"assets\":{\"size\":1}}," +                 // no names, tag ok: unusable
        sver("1.2.0", "stable", "official", "", 0) + "," +                       // zero-size image
        sver("1.3.0", "stable", "official", "", 1, 0))));                        // zero-size assets
    REQUIRE(s->kitCount == 1);
    REQUIRE(s->kits[0].count == 1);
    CHECK(std::strcmp(s->kits[0].versions[0].version, "1.0.0") == 0);
}

TEST_CASE("an icon over the cap is dropped, at the cap it stays", "[fw]") {
    auto icon = [](uint32_t size) {
        return ",\"icon\":{\"name\":\"i.bin\",\"size\":" + std::to_string(size) + "}";
    };
    auto s = load(envelope(skit("full", sver("1.2.0", "stable", "official", icon(kFwIconBytesMax)) + "," +
                                        sver("1.1.0", "stable", "official", icon(kFwIconBytesMax + 1)))));
    REQUIRE(s->kits[0].count == 2);
    CHECK(s->kits[0].versions[0].hasIcon);
    CHECK(s->kits[0].versions[0].bytes == 3000000u + kFwIconBytesMax);
    CHECK_FALSE(s->kits[0].versions[1].hasIcon);
    CHECK(s->kits[0].versions[1].bytes == 3000000u);
}

TEST_CASE("sizes add up saturating, a negative fetch time reads 0", "[fw]") {
    std::string json = envelope(skit("full", sver("1.0.0", "stable", "official", "", 4000000000u, 4000000000u)));
    auto s = load(json);
    CHECK(s->kits[0].versions[0].bytes == UINT32_MAX);
    json.replace(json.find("1790964000"), 10, "-5");
    CHECK(load(json)->fetched == 0);
}

TEST_CASE("store cache caps at 16 kits of 8 versions", "[fw]") {
    std::string kits;
    for (char c = 'a'; c < 'a' + 18; ++c) {
        std::string versions;
        for (int v = 0; v < 10; ++v) versions += (v ? "," : "") + sver("1.0." + std::to_string(v), "stable");
        kits += (kits.empty() ? "" : ",") + skit(std::string("k") + c, versions);
    }
    auto s = load(envelope(kits));
    CHECK(s->kitCount == kFwCatalogKits);
    CHECK(s->kits[0].count == kFwCatalogVersions);
}

TEST_CASE("the stream parsers agree with the buffer ones", "[fw]") {
    std::istringstream in(kTwo);
    auto s = std::make_unique<FwStore>();
    REQUIRE(parseFwStoreCacheFrom(in, 2, *s) == FwStoreLoad::Ok);
    CHECK(s->kitCount == 3);
    CHECK(s->kits[0].versions[0].bytes == 3000000u);
    std::istringstream bad("{\"v\":1,");
    CHECK(parseFwStoreCacheFrom(bad, 2, *s) == FwStoreLoad::Format);
    CHECK(s->kitCount == 0);

    const std::string kitObj = skit("drums", sver("1.3.0", "stable", "official", kDetail));
    std::istringstream detail(kitObj);
    FwKitDetail d{};
    REQUIRE(parseFwKitDetailFrom(detail, "1.3.0", d));
    CHECK(std::strcmp(d.displayName, "Drum Machine") == 0);
}

TEST_CASE("a failed detail parse leaves the output byte for byte alone", "[fw]") {
    const std::string kitObj = skit("drums", sver("1.3.0", "stable"));
    FwKitDetail d;
    std::memset(&d, 0xAB, sizeof d);
    FwKitDetail before = d;
    CHECK_FALSE(parseFwKitDetail(kitObj.data(), kitObj.size(), "2.0.0", d));
    CHECK_FALSE(parseFwKitDetail("{", 1, "1.3.0", d));
    CHECK_FALSE(parseFwManifestDetail("{", 1, d));
    CHECK(std::memcmp(&d, &before, sizeof d) == 0);
}

TEST_CASE("app ids are deduplicated", "[fw]") {
    const std::string kitObj = skit("drums", sver("1.3.0", "stable", "official",
                                                  ",\"apps\":[\"sampler\",\"sampler\",\"pad-mixer\"]"));
    FwKitDetail d{};
    REQUIRE(parseFwKitDetail(kitObj.data(), kitObj.size(), "1.3.0", d));
    CHECK(d.appCount == 2);
}

TEST_CASE("fwCopyText replaces invalid UTF-8 and drops control characters", "[fw]") {
    char out[32];
    auto copy = [&](const char* in) {
        fwCopyText(out, sizeof out, in);
        return std::string(out);
    };
    CHECK(copy("\xC0\x80") == "?");                        // overlong NUL
    CHECK(copy("\xE0\x80\x80") == "?");                    // overlong 3-byte
    CHECK(copy("\xED\xA0\x80") == "?");                    // surrogate
    CHECK(copy("\xF4\x90\x80\x80") == "?");                // past U+10FFFF
    CHECK(copy("\xFF") == "?");
    CHECK(copy("\x80z") == "?z");                           // stray continuation
    CHECK(copy("ab\xE2\x82") == "ab?");                     // truncated at the end
    CHECK(copy("\xE2\x82" "x") == "?x");                    // truncated in the middle
    CHECK(copy("a\x1b[0m") == "a[0m");
    CHECK(copy("a\r\nb\tc\x7f") == "a\nbc");
    CHECK(copy("z\xC5\x82\xE2\x82\xAC\xF0\x9F\x98\x80") == "z\xC5\x82\xE2\x82\xAC\xF0\x9F\x98\x80");
    char one[1] = {'x'};
    fwCopyText(one, 1, "abc");
    CHECK(one[0] == '\0');
    char none[1] = {'x'};
    fwCopyText(none, 0, "abc");
    CHECK(none[0] == 'x');
    char tight[4];
    fwCopyText(tight, sizeof tight, "a\xFF\xFF" "bcd");   // '?' counts against the room like any byte
    CHECK(std::strcmp(tight, "a??") == 0);
}

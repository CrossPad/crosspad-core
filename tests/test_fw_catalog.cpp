#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <memory>
#include <string>
#include "crosspad/fw/FwCatalog.hpp"
#include "crosspad/fw/FwManifest.hpp"

using namespace crosspad;

static std::string ver(const std::string& kit, const std::string& version, const char* channel,
                       const char* tier = "official") {
    const std::string tag = "v" + version;
    const std::string base = "CrossPad-" + tag + "-v2-" + kit;
    return "{\"version\":\"" + version + "\",\"tag\":\"" + tag + "\",\"channel\":\"" + channel +
           "\",\"tier\":\"" + tier + "\",\"bin\":{\"name\":\"" + base +
           ".bin\",\"size\":2568208},\"assets\":{\"name\":\"" + base +
           "-assets.bin\",\"size\":1048576},\"manifest\":\"" + base + "-manifest.json\",\"sig\":\"" +
           base + "-manifest.sig\"}";
}

static std::string kitJson(const std::string& kit, const std::string& versions) {
    return "{\"kit\":\"" + kit + "\",\"versions\":[" + versions + "]}";
}

static std::string catalog(const std::string& kits) {
    return "{\"v\":1,\"generated\":\"2026-10-02T18:00:00Z\",\"kits\":[" + kits + "]}";
}

static std::unique_ptr<FwCatalog> parsed(const std::string& json) {
    auto c = std::make_unique<FwCatalog>();
    REQUIRE(parseFwCatalog(json.data(), json.size(), *c));
    return c;
}

static Semver sv(const char* s) {
    Semver v;
    REQUIRE(Semver::parse(s, v));
    return v;
}

static const std::string kTwoKits = catalog(
    kitJson("full", ver("full", "1.4.0-rc1", "beta") + "," + ver("full", "1.3.0", "stable") + "," +
                        ver("full", "1.2.0", "stable")) +
    "," + kitJson("lite", ver("lite", "1.3.0", "stable", "community")));

TEST_CASE("catalog parses kits, versions, files", "[fw]") {
    auto c = parsed(kTwoKits);
    REQUIRE(c->kitCount == 2);
    CHECK(std::strcmp(c->kits[0].kit, "full") == 0);
    REQUIRE(c->kits[0].count == 3);
    const FwCatalogVersion& v = c->kits[0].versions[1];
    CHECK(std::strcmp(v.version, "1.3.0") == 0);
    CHECK(std::strcmp(v.tag, "v1.3.0") == 0);
    CHECK(v.channel == FwChannel::Stable);
    CHECK(v.tier == FwTier::Official);
    CHECK(std::strcmp(v.bin.name, "CrossPad-v1.3.0-v2-full.bin") == 0);
    CHECK(v.bin.size == 2568208);
    CHECK(v.assets.size == 1048576);
    CHECK(std::strcmp(v.sig, "CrossPad-v1.3.0-v2-full-manifest.sig") == 0);
    CHECK(c->kits[1].versions[0].tier == FwTier::Community);
}

TEST_CASE("update pick: newer only, channel rules, skip list", "[fw]") {
    auto c = parsed(kTwoKits);
    const FwCatalogVersion* u = fwCatalogPickUpdate(*c, "full", FwChannel::Stable, sv("1.2.0"));
    REQUIRE(u);
    CHECK(std::strcmp(u->version, "1.3.0") == 0);
    u = fwCatalogPickUpdate(*c, "full", FwChannel::Beta, sv("1.2.0"));
    REQUIRE(u);
    CHECK(std::strcmp(u->version, "1.4.0-rc1") == 0);
    CHECK_FALSE(fwCatalogPickUpdate(*c, "full", FwChannel::Stable, sv("1.3.0")));
    CHECK_FALSE(fwCatalogPickUpdate(*c, "full", FwChannel::Beta, sv("1.4.0-rc1")));
    CHECK_FALSE(fwCatalogPickUpdate(*c, "full", FwChannel::Stable, sv("v1.3.0-12-gabcdef0")));
    CHECK(fwCatalogPickUpdate(*c, "full", FwChannel::Beta, sv("v1.3.0-12-gabcdef0")));   // rc1 of 1.4.0 is newer
    auto skip130 = [](const char*, const char* version, void*) { return std::strcmp(version, "1.3.0") == 0; };
    CHECK_FALSE(fwCatalogPickUpdate(*c, "full", FwChannel::Stable, sv("1.2.0"), skip130, nullptr));
    u = fwCatalogPickUpdate(*c, "lite", FwChannel::Stable, sv("1.0.0"));
    REQUIRE(u);
    CHECK(u->tier == FwTier::Community);
    CHECK_FALSE(fwCatalogPickUpdate(*c, "daw", FwChannel::Beta, sv("0.1.0")));
    REQUIRE(fwCatalogFind(*c, "full", "v1.2.0"));
    CHECK_FALSE(fwCatalogFind(*c, "full", "1.9.9"));
}

TEST_CASE("catalog refuses the wrong format, keeps nothing", "[fw]") {
    auto c = std::make_unique<FwCatalog>();
    for (const std::string bad : {std::string(""), std::string("not json"), std::string("{\"v\":2,\"kits\":[]}"),
                                  std::string("{\"v\":1}")}) {
        c->kitCount = 7;
        CHECK_FALSE(parseFwCatalog(bad.data(), bad.size(), *c));
        CHECK(c->kitCount == 0);
    }
    CHECK_FALSE(parseFwCatalog(nullptr, 0, *c));
}

TEST_CASE("catalog skips entries it cannot use safely", "[fw]") {
    std::string good = ver("full", "1.3.0", "stable");
    std::string slash = good;
    slash.replace(slash.find("CrossPad-v1.3.0-v2-full.bin"), 8, "../evil/");
    std::string space = good;
    space.replace(space.find("-assets.bin"), 1, " ");
    std::string longTag = ver("full", "1.3.0-" + std::string(30, 'a'), "beta");
    std::string longTagOnly = good;
    longTagOnly.replace(longTagOnly.find("\"tag\":\"v1.3.0\""), 14, "\"tag\":\"v" + std::string(40, 'a') + "\"");
    std::string badChannel = ver("full", "1.3.1", "nightly");
    std::string badTier = ver("full", "1.3.2", "stable", "vendor");
    std::string badVersion = ver("full", "one", "stable");
    std::string noSize = good;
    noSize.replace(noSize.find("2568208"), 7, "0");
    auto c = parsed(catalog(
        kitJson("full", slash + "," + space + "," + longTag + "," + longTagOnly + "," + badChannel + "," + badTier + "," +
                            badVersion + "," + noSize + "," + good) +
        "," + kitJson("Bad Kit", good) + "," + kitJson("full", ver("full", "9.9.9", "stable")) + "," +
        kitJson("empty", badChannel)));
    REQUIRE(c->kitCount == 1);                 // "Bad Kit" invalid, duplicate "full" dropped, "empty" has no valid version
    REQUIRE(c->kits[0].count == 1);
    CHECK(std::strcmp(c->kits[0].versions[0].version, "1.3.0") == 0);
}

TEST_CASE("catalog caps kits and versions", "[fw]") {
    std::string kits;
    for (int k = 0; k < 20; ++k) {
        std::string versions;
        for (int i = 0; i < 12; ++i) {
            if (i) versions += ",";
            versions += ver("k" + std::to_string(k), "1.0." + std::to_string(i), "stable");
        }
        if (k) kits += ",";
        kits += kitJson("k" + std::to_string(k), versions);
    }
    auto c = parsed(catalog(kits));
    CHECK(c->kitCount == kFwCatalogKits);
    CHECK(c->kits[0].count == kFwCatalogVersions);
    CHECK(std::strcmp(c->kits[0].versions[7].version, "1.0.7") == 0);   // the first eight are kept
}

static const char* kManifest = R"({
  "name": "full-1.3.0", "kit": "full", "version": "v1.3.0", "board_rev": 2,
  "stm_proto_min": 15, "assets_id": "00", "channel": "stable", "tier": "official", "tag": "v1.3.0",
  "files": {
    "CrossPad.bin": {"size": 2568208, "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},
    "assets.bin": {"size": 1048576, "sha256": "ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789"}
  }
})";

TEST_CASE("manifest parses a CI package and matches catalog and board", "[fw]") {
    FwManifest m;
    REQUIRE(parseFwManifest(kManifest, std::strlen(kManifest), m));
    CHECK(std::strcmp(m.kit, "full") == 0);
    CHECK(m.boardRev == 2);
    CHECK(m.channel == FwChannel::Stable);
    CHECK(std::strcmp(m.tag, "v1.3.0") == 0);
    const FwManifestFile* bin = fwManifestFile(m, "CrossPad.bin");
    REQUIRE(bin);
    CHECK(bin->size == 2568208);
    REQUIRE(fwManifestFile(m, "assets.bin"));
    CHECK_FALSE(fwManifestFile(m, "settings.json"));
    CHECK(fwManifestMatches(m, "full", "1.3.0", 2));
    CHECK_FALSE(fwManifestMatches(m, "lite", "1.3.0", 2));
    CHECK_FALSE(fwManifestMatches(m, "full", "1.3.1", 2));
    CHECK_FALSE(fwManifestMatches(m, "full", "1.3.0", 1));
    CHECK_FALSE(fwManifestMatches(m, "full", "garbage", 2));
}

TEST_CASE("manifest from before channels still parses; broken ones do not", "[fw]") {
    const char* old = R"({"name":"x","kit":"full","version":"1.0.0","board_rev":2,
      "files":{"CrossPad.bin":{"size":10,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"}}})";
    FwManifest m;
    REQUIRE(parseFwManifest(old, std::strlen(old), m));
    CHECK(m.channel == FwChannel::Stable);
    CHECK(m.tier == FwTier::Official);
    CHECK(m.tag[0] == '\0');
    const char* noImage = R"({"name":"x","kit":"full","version":"1.0.0","board_rev":2,"files":{}})";
    CHECK_FALSE(parseFwManifest(noImage, std::strlen(noImage), m));
    const char* shortSha = R"({"name":"x","kit":"full","version":"1.0.0","board_rev":2,
      "files":{"CrossPad.bin":{"size":10,"sha256":"0123"}}})";
    CHECK_FALSE(parseFwManifest(shortSha, std::strlen(shortSha), m));
    const char* noRev = R"({"name":"x","kit":"full","version":"1.0.0",
      "files":{"CrossPad.bin":{"size":10,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"}}})";
    CHECK_FALSE(parseFwManifest(noRev, std::strlen(noRev), m));
    CHECK_FALSE(parseFwManifest("{", 1, m));
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include "crosspad/net/WifiTypes.hpp"
#include "crosspad/net/WifiNetworkList.hpp"
#include "crosspad/util/QuotedArgs.hpp"

using namespace crosspad;

TEST_CASE("wifiRssiBars thresholds", "[wifi]") {
    CHECK(wifiRssiBars(-40) == 4);
    CHECK(wifiRssiBars(-55) == 4);
    CHECK(wifiRssiBars(-56) == 3);
    CHECK(wifiRssiBars(-65) == 3);
    CHECK(wifiRssiBars(-66) == 2);
    CHECK(wifiRssiBars(-75) == 2);
    CHECK(wifiRssiBars(-76) == 1);
    CHECK(wifiRssiBars(-127) == 1);
}

TEST_CASE("state and fail names", "[wifi]") {
    CHECK(std::strcmp(wifiLinkStateName(WifiLinkState::Up), "up") == 0);
    CHECK(std::strcmp(wifiLinkStateName(WifiLinkState::Failed), "failed") == 0);
    CHECK(std::strcmp(wifiFailName(WifiFail::NoMem), "no_mem") == 0);
    CHECK(std::strcmp(wifiFailName(WifiFail::TransferMode), "transfer_mode") == 0);
}

TEST_CASE("put adds at the front, updates in place, caps at kMax", "[wifi]") {
    WifiNetworkList l;
    REQUIRE(l.put("a", "1"));
    REQUIRE(l.put("b", "2"));
    CHECK(l.size() == 2);
    CHECK(std::strcmp(l.at(0).ssid, "b") == 0);
    REQUIRE(l.put("a", "new"));                 // existing: moved to front, password replaced
    CHECK(l.size() == 2);
    CHECK(std::strcmp(l.at(0).ssid, "a") == 0);
    CHECK(std::strcmp(l.at(0).pass, "new") == 0);
    for (const char* s : {"c", "d", "e", "f"}) REQUIRE(l.put(s, "x"));
    CHECK(l.size() == WifiNetworkList::kMax);   // oldest ("b") dropped
    CHECK(l.find("b") < 0);
    CHECK(std::strcmp(l.at(0).ssid, "f") == 0);
}

TEST_CASE("put rejects empty or oversize fields", "[wifi]") {
    WifiNetworkList l;
    std::string ssid33(33, 's'), pass65(65, 'p');
    CHECK_FALSE(l.put("", "x"));
    CHECK_FALSE(l.put(ssid33.c_str(), "x"));
    CHECK_FALSE(l.put("ok", pass65.c_str()));
    CHECK(l.put(std::string(32, 's').c_str(), std::string(64, 'p').c_str()));
    CHECK(l.put("open", ""));                   // open network: empty password allowed
}

TEST_CASE("promote and forget", "[wifi]") {
    WifiNetworkList l;
    l.put("a", "1"); l.put("b", "2"); l.put("c", "3");   // order: c b a
    REQUIRE(l.promote("a"));
    CHECK(std::strcmp(l.at(0).ssid, "a") == 0);
    CHECK(std::strcmp(l.at(1).ssid, "c") == 0);
    CHECK_FALSE(l.promote("zzz"));
    REQUIRE(l.forget("c"));
    CHECK(l.size() == 2);
    CHECK_FALSE(l.forget("c"));
}

TEST_CASE("blob round-trip keeps order, spaces, UTF-8 and empty passwords", "[wifi]") {
    WifiNetworkList l;
    l.put("Dom Kowalskich", "haslo 123");
    l.put("\xC5\xBB\xC3\xB3\xC5\x82ta sie\xC4\x87", "");   // "Żółta sieć", open
    uint8_t buf[WifiNetworkList::kBlobMax];
    size_t n = l.serialize(buf, sizeof(buf));
    REQUIRE(n > 0);
    WifiNetworkList r;
    REQUIRE(r.deserialize(buf, n));
    REQUIRE(r.size() == 2);
    CHECK(std::strcmp(r.at(0).ssid, "\xC5\xBB\xC3\xB3\xC5\x82ta sie\xC4\x87") == 0);
    CHECK(r.at(0).pass[0] == '\0');
    CHECK(std::strcmp(r.at(1).ssid, "Dom Kowalskich") == 0);
    CHECK(std::strcmp(r.at(1).pass, "haslo 123") == 0);
}

TEST_CASE("deserialize rejects corrupt blobs and leaves the list empty", "[wifi]") {
    WifiNetworkList l;
    l.put("a", "1");
    uint8_t buf[WifiNetworkList::kBlobMax];
    size_t n = l.serialize(buf, sizeof(buf));

    WifiNetworkList r;
    uint8_t bad[WifiNetworkList::kBlobMax];
    std::memcpy(bad, buf, n);
    bad[0] = 99;                                  // unknown version
    CHECK_FALSE(r.deserialize(bad, n));
    CHECK(r.size() == 0);
    CHECK_FALSE(r.deserialize(buf, n - 1));       // truncated
    CHECK(r.size() == 0);
    std::memcpy(bad, buf, n);
    bad[1] = 6;                                   // count past kMax
    CHECK_FALSE(r.deserialize(bad, n));
    std::memcpy(bad, buf, n);
    bad[2] = 40;                                  // ssid length past kMaxSsid
    CHECK_FALSE(r.deserialize(bad, n));
    uint8_t extra[WifiNetworkList::kBlobMax + 1];
    std::memcpy(extra, buf, n); extra[n] = 0;
    CHECK_FALSE(r.deserialize(extra, n + 1));     // trailing bytes
    CHECK_FALSE(r.deserialize(nullptr, 0));
}

TEST_CASE("serialize refuses a buffer that is too small", "[wifi]") {
    WifiNetworkList l;
    l.put("abc", "def");
    uint8_t buf[4];
    CHECK(l.serialize(buf, sizeof(buf)) == 0);
}

TEST_CASE("nextQuotedArg splits plain and quoted tokens", "[wifi]") {
    const char* p = "  \"Dom Kowalskich\" haslo123  plain";
    char out[65];
    REQUIRE(nextQuotedArg(p, out, sizeof(out)));
    CHECK(std::strcmp(out, "Dom Kowalskich") == 0);
    REQUIRE(nextQuotedArg(p, out, sizeof(out)));
    CHECK(std::strcmp(out, "haslo123") == 0);
    REQUIRE(nextQuotedArg(p, out, sizeof(out)));
    CHECK(std::strcmp(out, "plain") == 0);
    CHECK_FALSE(nextQuotedArg(p, out, sizeof(out)));
}

TEST_CASE("nextQuotedArg: empty quotes, unterminated quote, overflow", "[wifi]") {
    char out[8];
    const char* p = "\"\" x";
    REQUIRE(nextQuotedArg(p, out, sizeof(out)));
    CHECK(out[0] == '\0');                        // "" = empty password
    const char* q = "\"unterminated";
    CHECK_FALSE(nextQuotedArg(q, out, sizeof(out)));
    const char* r = "toolongtoken";
    CHECK_FALSE(nextQuotedArg(r, out, sizeof(out)));
}

TEST_CASE("not_network_mode is named and appended", "[wifi]") {
    CHECK(std::strcmp(wifiFailName(WifiFail::NotNetworkMode), "not_network_mode") == 0);
    CHECK(static_cast<int>(WifiFail::NotNetworkMode) == static_cast<int>(WifiFail::TransferMode) + 1);
}

TEST_CASE("bars look follows the spec table", "[wifi]") {
    CHECK_FALSE(wifiBarsLook(false, WifiLinkState::Up, -40, 0).visible);   // not allowed: hidden

    const auto asleep = wifiBarsLook(true, WifiLinkState::Off, 0, 0);       // allowed, radio asleep
    CHECK(asleep.visible);
    CHECK(asleep.lit == 0);
    CHECK_FALSE(asleep.firstRed);
    CHECK(wifiBarsLook(true, WifiLinkState::Stopping, 0, 0).lit == 0);

    CHECK(wifiBarsLook(true, WifiLinkState::Up, -54, 0).lit == 4);
    CHECK(wifiBarsLook(true, WifiLinkState::Up, -70, 0).lit == 2);
    CHECK(wifiBarsLook(true, WifiLinkState::Up, -80, 0).lit == 1);

    const auto failed = wifiBarsLook(true, WifiLinkState::Failed, 0, 0);
    CHECK(failed.visible);
    CHECK(failed.lit == 0);
    CHECK(failed.firstRed);
}

TEST_CASE("connecting sweeps one bar per step, looping 1..4", "[wifi]") {
    for (auto st : {WifiLinkState::Starting, WifiLinkState::Connecting}) {
        CHECK(wifiBarsLook(true, st, 0, 0).lit == 1);
        CHECK(wifiBarsLook(true, st, 0, kWifiConnectStepMs - 1).lit == 1);
        CHECK(wifiBarsLook(true, st, 0, kWifiConnectStepMs).lit == 2);
        CHECK(wifiBarsLook(true, st, 0, 3 * kWifiConnectStepMs).lit == 4);
        CHECK(wifiBarsLook(true, st, 0, 4 * kWifiConnectStepMs).lit == 1);
        CHECK_FALSE(wifiBarsLook(true, st, 0, 0).firstRed);
    }
}

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include "crosspad/fw/FwAccount.hpp"

using namespace crosspad;

TEST_CASE("the token goes only under the account base", "[fw]") {
    const char* base = "https://crosspad.app";
    CHECK(fwAccountUrlAllowed(base, "https://crosspad.app/api/device/catalog?rev=v2"));
    CHECK(fwAccountUrlAllowed(base, "https://crosspad.app/api/device/file/b-1a2b/x.bin"));
    CHECK_FALSE(fwAccountUrlAllowed(base, "https://crosspad.app"));              // no path: nothing to authorise
    CHECK_FALSE(fwAccountUrlAllowed(base, "https://crosspad.app.evil.com/api"));
    CHECK_FALSE(fwAccountUrlAllowed(base, "https://crosspad.application/api"));
    CHECK_FALSE(fwAccountUrlAllowed(base, "https://crosspad.app@evil.com/api"));
    CHECK_FALSE(fwAccountUrlAllowed(base, "http://crosspad.app/api"));           // scheme is part of the base
    CHECK_FALSE(fwAccountUrlAllowed(base, "HTTPS://CROSSPAD.APP/api"));          // compared byte for byte
    CHECK_FALSE(fwAccountUrlAllowed(base, nullptr));
    CHECK_FALSE(fwAccountUrlAllowed("", "https://crosspad.app/api"));
    CHECK(fwAccountUrlAllowed("http://192.0.2.1:8765", "http://192.0.2.1:8765/api/device/catalog?rev=v2"));
    CHECK_FALSE(fwAccountUrlAllowed("http://192.0.2.1:8765", "http://192.0.2.1:87650/api"));
    CHECK_FALSE(fwAccountUrlAllowed("https://crosspad.app/", "https://crosspad.app//api"));   // a base with a path is not a base
}

TEST_CASE("codes and tokens have one shape", "[fw]") {
    CHECK(fwPairCodeValid("K7M2QX"));
    CHECK_FALSE(fwPairCodeValid("K7M2Q"));
    CHECK_FALSE(fwPairCodeValid("K7M2QX1"));
    CHECK_FALSE(fwPairCodeValid("K7M2Q0"));   // 0, O, 1, I are not in the alphabet
    CHECK_FALSE(fwPairCodeValid("k7m2qx"));
    const std::string tok(64, 'a');
    CHECK(fwDeviceTokenValid(tok.c_str()));
    CHECK_FALSE(fwDeviceTokenValid(std::string(63, 'a').c_str()));
    CHECK_FALSE(fwDeviceTokenValid((std::string(63, 'a') + "G").c_str()));
    CHECK_FALSE(fwDeviceTokenValid(std::string(64, 'A').c_str()));   // lower-case hex only
}

TEST_CASE("pair replies", "[fw]") {
    FwPairStart s{};
    const std::string poll(64, 'a');
    const std::string start = R"({"code":"K7M2QX","poll":")" + poll + R"(","expires_in":600})";
    REQUIRE(parseFwPairStart(start.data(), start.size(), s));
    CHECK(std::strcmp(s.code, "K7M2QX") == 0);
    CHECK(std::string(s.poll) == poll);
    CHECK(s.expiresIn == 600);
    const std::string badCode = R"({"code":"K7M2Q0","poll":")" + poll + R"(","expires_in":600})";
    CHECK_FALSE(parseFwPairStart(badCode.data(), badCode.size(), s));
    // No poll secret, or a malformed one: the server is not one this board pairs with.
    const std::string noPoll = R"({"code":"K7M2QX","expires_in":600})";
    CHECK_FALSE(parseFwPairStart(noPoll.data(), noPoll.size(), s));
    const std::string badPoll = R"({"code":"K7M2QX","poll":")" + std::string(64, 'G') + R"(","expires_in":600})";
    CHECK_FALSE(parseFwPairStart(badPoll.data(), badPoll.size(), s));

    FwPairToken t{};
    const std::string waiting = R"({"status":"waiting"})";
    CHECK(parseFwPairToken(202, waiting.data(), waiting.size(), t) == FwPairReply::Waiting);
    CHECK(parseFwPairToken(410, "", 0, t) == FwPairReply::Expired);
    const std::string ok = R"({"token":")" + std::string(64, 'b') + R"(","account":"Ola Ł.\nX"})";
    REQUIRE(parseFwPairToken(200, ok.data(), ok.size(), t) == FwPairReply::Token);
    CHECK(std::string(t.token) == std::string(64, 'b'));
    CHECK(std::string(t.account) == "Ola \xC5\x81.X");   // one line, UTF-8 kept (fwCopyLine)
    const std::string shortTok = R"({"token":"abc","account":"x"})";
    CHECK(parseFwPairToken(200, shortTok.data(), shortTok.size(), t) == FwPairReply::Bad);
    CHECK(parseFwPairToken(500, "", 0, t) == FwPairReply::Bad);
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "crosspad/net/RemoteFrame.hpp"
#include <cstring>
#include <string>

using namespace crosspad;

static bool parse(const std::string& s, RemoteRequest& r) { return parseRemoteRequest(s.data(), s.size(), r); }

TEST_CASE("RemoteFrame: request splits id and command", "[remote]") {
    RemoteRequest r{};
    REQUIRE(parse("a1 KIT_LIST", r));
    REQUIRE(std::string(r.id) == "a1");
    REQUIRE(std::string(r.command) == "KIT_LIST");
    REQUIRE(parse("z ENC_ROTATE 1 -2", r));
    REQUIRE(std::string(r.command) == "ENC_ROTATE 1 -2");
}

TEST_CASE("RemoteFrame: bad requests are refused", "[remote]") {
    RemoteRequest r{};
    REQUIRE_FALSE(parse("", r));
    REQUIRE_FALSE(parse("a1", r));                 // no command
    REQUIRE_FALSE(parse("a1 ", r));                // empty command
    REQUIRE_FALSE(parse(" KIT_LIST", r));          // empty id
    REQUIRE_FALSE(parse("A1 KIT_LIST", r));        // id is [0-9a-z]
    REQUIRE_FALSE(parse("123456789 X", r));        // id longer than 8
    REQUIRE_FALSE(parse("a1 KIT\nLIST", r));       // control byte
    REQUIRE_FALSE(parse("a1 KIT\x7f", r));
    REQUIRE_FALSE(parseRemoteRequest(nullptr, 0, r));
    std::string big = "a1 " + std::string(kRemoteCommandMax + 1, 'X');
    REQUIRE_FALSE(parse(big, r));
    std::string max = "a1 " + std::string(kRemoteCommandMax, 'X');
    REQUIRE(parse(max, r));
    REQUIRE(std::strlen(r.command) == kRemoteCommandMax);
}

TEST_CASE("RemoteFrame: reply is prefixed with the id", "[remote]") {
    char out[64];
    size_t n = formatRemoteReply("a1", "KITS: 3\r\n", out, sizeof out);
    REQUIRE(std::string(out, n) == "a1 KITS: 3\r\n");
    REQUIRE(out[n] == '\0');
}

TEST_CASE("RemoteFrame: a reply too long is cut at a line and marked", "[remote]") {
    std::string reply;
    for (int i = 0; reply.size() < kRemoteReplyMax + 200; ++i) reply += "KIT: " + std::to_string(i) + " name\r\n";
    static char out[kRemoteIdMax + 2 + kRemoteReplyMax + 1];
    size_t n = formatRemoteReply("q", reply.c_str(), out, sizeof out);
    std::string s(out, n);
    REQUIRE(s.rfind("q ", 0) == 0);
    REQUIRE(n <= sizeof out - 1);
    REQUIRE(s.size() >= 15);
    REQUIRE(s.substr(s.size() - 15) == "ERR truncated\r\n");
    std::string body = s.substr(2, s.size() - 2 - 15);
    REQUIRE(body.size() >= 2);
    REQUIRE(body.substr(body.size() - 2) == "\r\n");   // cut at a line boundary
}

TEST_CASE("RemoteFrame: tiny buffers never overflow", "[remote]") {
    char out[4];
    size_t n = formatRemoteReply("abcdefgh", "OK\r\n", out, sizeof out);
    REQUIRE(n < sizeof out);
    REQUIRE(out[n] == '\0');
    REQUIRE(formatRemoteReply("a", "OK", out, 0) == 0);
}

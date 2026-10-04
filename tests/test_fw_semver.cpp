#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include "crosspad/fw/HttpDate.hpp"
#include "crosspad/fw/Semver.hpp"

using namespace crosspad;

static Semver v(const char* s) {
    Semver out;
    REQUIRE(Semver::parse(s, out));
    return out;
}

TEST_CASE("Semver parses releases, a leading v and a missing patch", "[fw]") {
    Semver s = v("v1.2.3");
    CHECK(s.major == 1);
    CHECK(s.minor == 2);
    CHECK(s.patch == 3);
    CHECK(s.pre[0] == '\0');
    CHECK(s.commits == 0);
    CHECK(v("1.2") == v("1.2.0"));
    CHECK(v("1.2.3+build.7") == v("1.2.3"));
    CHECK(v("1.2.3-rc1+build.7") == v("1.2.3-rc1"));
}

TEST_CASE("Semver parses pre-releases and git describe builds", "[fw]") {
    CHECK(std::strcmp(v("1.3.0-rc1").pre, "rc1") == 0);
    Semver dev = v("v1.1.0-rc1-64-gc6d227d");
    CHECK(std::strcmp(dev.pre, "rc1") == 0);
    CHECK(dev.commits == 64);
    Semver post = v("v1.0.1-284-gd676096");
    CHECK(post.pre[0] == '\0');
    CHECK(post.commits == 284);
    Semver dirty = v("v1.1.0-rc1-62-gce30cc8-dirty");
    CHECK(std::strcmp(dirty.pre, "rc1") == 0);
    CHECK(dirty.commits == 62);
    CHECK(v("1.2.3-dirty") == v("1.2.3"));
    CHECK(std::strcmp(v("1.0.0-alpha-gamma").pre, "alpha-gamma") == 0);   // "gamma" is not a hash
}

TEST_CASE("Semver rejects what is not a version", "[fw]") {
    Semver s;
    for (const char* bad : {"", "v", "1", "1.", "a.b.c", "1.2.3.4", "1.2.3-", "1.2.3-rc 1",
                            "1.2.3-rc..1", "70000.0.0", "c6d227d", "1.2.3-abcdefghijklmnopqrstuvwxyz"})
        CHECK_FALSE(Semver::parse(bad, s));
    CHECK_FALSE(Semver::parse(nullptr, s));
}

TEST_CASE("Semver checks +build like a pre-release", "[fw]") {
    Semver s;
    for (const char* bad : {"9.9.9+a b", "9.9.9+a\r\nFWUPD: state=x", "9.9.9+a/b", "9.9.9+%41", "9.9.9+",
                            "9.9.9+a..b", "9.9.9-rc1+a b", "9.9.9-rc1+x/y", "9.9.9-rc1+"})
        CHECK_FALSE(Semver::parse(bad, s));
}

TEST_CASE("Semver order: pre-release below release, describe builds in between", "[fw]") {
    CHECK(v("1.2.9") < v("1.3.0"));
    CHECK(v("1.3.0-rc1") < v("1.3.0"));
    CHECK(v("1.3.0-rc1") < v("1.3.0-rc2"));
    CHECK(v("1.3.0-rc9") < v("1.3.0-rc10"));          // same letters: the digits as a number
    CHECK(v("1.3.0-beta.2") < v("1.3.0-beta.10"));    // numeric identifiers
    CHECK(v("1.3.0-alpha") < v("1.3.0-alpha.1"));     // more identifiers sort higher
    CHECK(v("1.3.0-1") < v("1.3.0-alpha"));           // numeric below alphanumeric
    CHECK(v("1.3.0-alpha") < v("1.3.0-beta"));
    CHECK(v("1.1.0-rc1") < v("v1.1.0-rc1-64-gc6d227d"));
    CHECK(v("v1.1.0-rc1-64-gc6d227d") < v("1.1.0-rc2"));
    CHECK(v("v1.1.0-rc1-64-gc6d227d") < v("1.1.0"));
    CHECK(v("1.0.1") < v("v1.0.1-284-gd676096"));
    CHECK(v("v1.0.1-284-gd676096") < v("1.0.2-rc1"));
    CHECK(v("v1.3.0") == v("1.3.0"));
}

TEST_CASE("compareVersionText puts unparseable text lowest", "[fw]") {
    CHECK(compareVersionText("1.0.0", "garbage") > 0);
    CHECK(compareVersionText("garbage", "1.0.0") < 0);
    CHECK(compareVersionText("garbage", "other") == 0);
    CHECK(compareVersionText("v1.3.0", "1.3.0") == 0);
}

TEST_CASE("parseHttpDate reads IMF-fixdate", "[fw]") {
    int64_t t = -1;
    REQUIRE(parseHttpDate("Thu, 01 Jan 1970 00:00:00 GMT", t));
    CHECK(t == 0);
    REQUIRE(parseHttpDate("Fri, 02 Oct 2026 18:00:00 GMT", t));
    CHECK(t == 1790964000);
    REQUIRE(parseHttpDate("Tue, 29 Feb 2028 23:59:59 GMT", t));
    CHECK(t == 1835481599);
    for (const char* bad : {"", "yesterday", "Fri, 02 Foo 2026 18:00:00 GMT",
                            "Fri, 32 Oct 2026 18:00:00 GMT", "Fri, 02 Oct 2026 24:00:00 GMT",
                            "Fri, 02 Oc 2026 18:00:00 GMT", "Fri, 02 Oct 2026 18:00:00 UTC",
                            "Fri, 02 Oct 2026 18:00:00 GMX", "Fri, 02 Oct 2026 18:00:00",
                            "Fri, 02 Oct 2026 18:00:00 GMT+1", "Fri, 02 Oct 2026 18:00:00 GM"})
        CHECK_FALSE(parseHttpDate(bad, t));
    CHECK_FALSE(parseHttpDate(nullptr, t));
}

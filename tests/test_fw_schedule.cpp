#include <catch2/catch_test_macros.hpp>
#include "crosspad/fw/FwSchedule.hpp"

using namespace crosspad;

TEST_CASE("daily check: due after 24 h of launcher idle, backs off after a failure", "[fw]") {
    const int64_t now = 1790964000;
    FwDailyInputs in{now, now - kFwDailyCheckS, kFwIdleBeforeCheckS, -1, false};
    CHECK(fwDailyCheckDue(in));
    in.idleS = kFwIdleBeforeCheckS - 1;
    CHECK_FALSE(fwDailyCheckDue(in));
    in.idleS = kFwIdleBeforeCheckS;
    in.lastCheckUnix = now - kFwDailyCheckS + 1;
    CHECK_FALSE(fwDailyCheckDue(in));
    in.lastCheckUnix = 0;                              // never checked
    CHECK(fwDailyCheckDue(in));
    in.sinceLastAttemptS = 10;
    in.lastAttemptFailed = true;                       // no WiFi / no_mem: wait an hour
    CHECK_FALSE(fwDailyCheckDue(in));
    in.sinceLastAttemptS = kFwRetryAfterFailS;
    CHECK(fwDailyCheckDue(in));
    in.lastAttemptFailed = false;                      // succeeded, but no Date header: a day
    CHECK_FALSE(fwDailyCheckDue(in));
    in.sinceLastAttemptS = kFwDailyCheckS;
    CHECK(fwDailyCheckDue(in));
}

TEST_CASE("daily check: an unset clock falls back to this boot's attempts", "[fw]") {
    const int64_t uptimeClock = 3600;                  // 1970 + uptime: never set
    FwDailyInputs in{uptimeClock, 1790964000, kFwIdleBeforeCheckS, -1, false};
    CHECK(fwDailyCheckDue(in));
    in.sinceLastAttemptS = 100;
    CHECK_FALSE(fwDailyCheckDue(in));
    in.nowUnix = 1790964000 - 10;                      // set, but behind last_check
    in.sinceLastAttemptS = -1;
    CHECK(fwDailyCheckDue(in));
}

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace crosspad {

/// When the daily background check is due (spec Revision 2): last_check a day
/// old (or unknown) and the launcher idle for two minutes. After a failed
/// attempt (no WiFi, no_mem) the next one waits an hour; after a good one
/// without a Date header, a day of this boot's own time.
constexpr int64_t kFwDailyCheckS = 24 * 3600;
constexpr int64_t kFwIdleBeforeCheckS = 120;
constexpr int64_t kFwRetryAfterFailS = 3600;
/// 2026-01-01T00:00:00Z: an earlier wall clock was never set (1970 + uptime).
constexpr int64_t kFwClockValidUnix = 1767225600;

struct FwDailyInputs {
    int64_t nowUnix;             ///< time(nullptr)
    int64_t lastCheckUnix;       ///< fwupd/last_check, 0 = never
    int64_t idleS;               ///< launcher in front with no pad press, seconds
    int64_t sinceLastAttemptS;   ///< since this boot's last check attempt, -1 = none yet
    bool lastAttemptFailed;
};

bool fwDailyCheckDue(const FwDailyInputs& in);

} // namespace crosspad

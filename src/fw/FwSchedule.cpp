#include "crosspad/fw/FwSchedule.hpp"

namespace crosspad {

bool fwDailyCheckDue(const FwDailyInputs& in) {
    if (in.idleS < kFwIdleBeforeCheckS) return false;
    if (in.sinceLastAttemptS >= 0) {
        // A success without a Date header leaves last_check stale; this boot's
        // own attempt times keep that from turning into a check per idle window.
        const int64_t gap = in.lastAttemptFailed ? kFwRetryAfterFailS : kFwDailyCheckS;
        if (in.sinceLastAttemptS < gap) return false;
    }
    if (in.nowUnix >= kFwClockValidUnix && in.lastCheckUnix > 0 && in.nowUnix >= in.lastCheckUnix)
        return in.nowUnix - in.lastCheckUnix >= kFwDailyCheckS;
    return true;
}

} // namespace crosspad

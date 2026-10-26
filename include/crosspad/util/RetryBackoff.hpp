#pragma once

#include <algorithm>
#include <cstdint>

namespace crosspad {

/// When to try something again that failed: at once the first time, then
/// kFirstMs after a failure, twice as long after each failure that follows,
/// kMostMs at most; a success starts it over. For work a failure must not
/// turn into a loop (a load that ran out of memory, a freeze the card
/// refused), on a millisecond clock that wraps.
class RetryBackoff {
public:
    static constexpr uint32_t kFirstMs = 2000;
    static constexpr uint32_t kMostMs = 60000;

    bool due(uint32_t nowMs) const { return !waitMs_ || (int32_t)(nowMs - atMs_) >= 0; }
    void failed(uint32_t nowMs)
    {
        waitMs_ = waitMs_ ? std::min(waitMs_ * 2, kMostMs) : kFirstMs;
        atMs_ = nowMs + waitMs_;
    }
    void reset() { waitMs_ = atMs_ = 0; }

private:
    uint32_t waitMs_ = 0;   ///< the wait the last failure set; 0: none failed
    uint32_t atMs_ = 0;
};

} // namespace crosspad

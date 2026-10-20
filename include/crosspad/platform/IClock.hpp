// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace crosspad {

/**
 * @brief Abstract clock interface for portable timing
 *
 * Platform implementations:
 *   - ESP-IDF: wraps esp_timer_get_time()
 *   - Arduino: wraps micros()
 *   - Linux: wraps std::chrono
 */
class IClock {
public:
    virtual ~IClock() = default;

    /// Microseconds since boot
    virtual int64_t getTimeUs() = 0;
};

/// The platform's clock.
IClock& getClock();

} // namespace crosspad

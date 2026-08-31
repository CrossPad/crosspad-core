#pragma once

#include <cstdint>

namespace crosspad {

/**
 * @brief Flags describing application behavior and state.
 *
 * Multiple flags can be OR-ed together.
 * Shared across ESP-IDF and Arduino platforms.
 */
enum AppFlags : uint8_t {
    APP_FLAG_NONE              = 0,
    APP_FLAG_MSGBOX            = 2,
    APP_FLAG_MAX               = UINT8_MAX
};

inline AppFlags operator|(AppFlags a, AppFlags b) {
    return static_cast<AppFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline AppFlags operator&(AppFlags a, AppFlags b) {
    return static_cast<AppFlags>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline AppFlags operator~(AppFlags a) {
    return static_cast<AppFlags>(~static_cast<uint8_t>(a));
}

} // namespace crosspad

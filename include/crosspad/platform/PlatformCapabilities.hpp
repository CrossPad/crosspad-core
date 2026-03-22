#pragma once

/**
 * @file PlatformCapabilities.hpp
 * @brief Platform capability flags — query what the current platform supports.
 *
 * Platforms set capability flags during init. Apps query them at runtime
 * via hasCapability() instead of null-checking interface pointers.
 * This is a query-only system — it does NOT enforce or filter anything.
 */

#include <cstdint>

namespace crosspad {

/**
 * @brief Flags describing hardware/software capabilities of the current platform.
 *
 * Multiple flags can be OR-ed together. Platforms call setPlatformCapabilities()
 * at init time and addPlatformCapability() as devices connect at runtime.
 */
enum class Capability : uint32_t {
    None        = 0,
    Midi        = 1 << 0,   ///< MIDI I/O available
    AudioOut    = 1 << 1,   ///< Audio output available
    AudioIn     = 1 << 2,   ///< Audio input available
    Synth       = 1 << 3,   ///< Synth engine available
    Pads        = 1 << 4,   ///< Pad grid available (physical or emulated)
    Leds        = 1 << 5,   ///< LED strip available
    Encoder     = 1 << 6,   ///< Rotary encoder available
    Display     = 1 << 7,   ///< LCD/display available
    Persistence = 1 << 8,   ///< Settings persistence (NVS, filesystem, etc.)
    Vibration   = 1 << 9,   ///< Haptic/vibration motor available
    WiFi        = 1 << 10,  ///< WiFi connectivity
    Bluetooth   = 1 << 11,  ///< Bluetooth connectivity
    Usb         = 1 << 12,  ///< USB host/device available
    Imu         = 1 << 13,  ///< IMU/accelerometer available
    Stm32       = 1 << 14,  ///< STM32 co-processor available
    Sequencer   = 1 << 15,  ///< Sequencer engine available
    Http        = 1 << 16,  ///< HTTP client available
};

// Bitwise operators
inline Capability operator|(Capability a, Capability b) {
    return static_cast<Capability>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline Capability operator&(Capability a, Capability b) {
    return static_cast<Capability>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline Capability operator~(Capability a) {
    return static_cast<Capability>(~static_cast<uint32_t>(a));
}

inline Capability& operator|=(Capability& a, Capability b) {
    a = a | b;
    return a;
}

inline Capability& operator&=(Capability& a, Capability b) {
    a = a & b;
    return a;
}

/// Set the platform's capability flags (call once during init)
void setPlatformCapabilities(Capability caps);

/// Add capability flag(s) at runtime (e.g., when MIDI connects after init)
void addPlatformCapability(Capability cap);

/// Remove capability flag(s) at runtime (e.g., when a device disconnects)
void removePlatformCapability(Capability cap);

/// Query whether ALL specified capabilities are present
bool hasCapability(Capability cap);

/// Query whether ANY of the specified capabilities are present
bool hasAnyCapability(Capability caps);

/// Get the raw capability flags
Capability getPlatformCapabilities();

} // namespace crosspad

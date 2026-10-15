// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace crosspad {

/**
 * @brief Abstract interface for key-value persistence
 *
 * Platform implementations:
 *   - ESP-IDF: NvsKeyValueStore (wraps NVS)
 *   - Arduino: PreferencesKeyValueStore (wraps NVS via Preferences.h)
 *   - Linux: FileKeyValueStore (wraps filesystem)
 */
class IKeyValueStore {
public:
    virtual ~IKeyValueStore() = default;

    /// Open the store. The platform calls it once at start-up.
    virtual bool init() = 0;

    // Per-key operations, each key scoped by a namespace @p ns
    /// Save a bool under @p ns / @p key.
    virtual void saveBool(const char* ns, const char* key, bool value) = 0;
    /// Save an 8-bit unsigned value under @p ns / @p key.
    virtual void saveU8(const char* ns, const char* key, uint8_t value) = 0;
    /// Save a 32-bit signed value under @p ns / @p key.
    virtual void saveI32(const char* ns, const char* key, int32_t value) = 0;

    /// The bool saved under @p ns / @p key, or @p defaultVal when none is.
    virtual bool readBool(const char* ns, const char* key, bool defaultVal) = 0;
    /// The 8-bit value saved under @p ns / @p key, or @p defaultVal when none is.
    virtual uint8_t readU8(const char* ns, const char* key, uint8_t defaultVal) = 0;
    /// The 32-bit value saved under @p ns / @p key, or @p defaultVal when none is.
    virtual int32_t readI32(const char* ns, const char* key, int32_t defaultVal) = 0;

    /// Erase all persisted data (factory reset)
    virtual void eraseAll() = 0;
};

} // namespace crosspad

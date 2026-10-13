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

    virtual bool init() = 0;

    // Per-key operations with namespace scoping
    virtual void saveBool(const char* ns, const char* key, bool value) = 0;
    virtual void saveU8(const char* ns, const char* key, uint8_t value) = 0;
    virtual void saveI32(const char* ns, const char* key, int32_t value) = 0;

    virtual bool readBool(const char* ns, const char* key, bool defaultVal) = 0;
    virtual uint8_t readU8(const char* ns, const char* key, uint8_t defaultVal) = 0;
    virtual int32_t readI32(const char* ns, const char* key, int32_t defaultVal) = 0;

    /// Erase all persisted data (factory reset)
    virtual void eraseAll() = 0;
};

} // namespace crosspad

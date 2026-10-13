// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file AppRegistry.hpp
 * @brief Portable application registry singleton
 *
 * Stores registered AppEntry descriptors. Pure C++ — no platform dependencies.
 * Platform-generated registration functions call registerApp() during startup.
 */

#include "crosspad/app/AppEntry.hpp"
#include <cstddef>

namespace crosspad {

/**
 * @brief Singleton registry of all known applications
 *
 * Thread-safety note: startup registration and runtime registration (e.g. Power
 * OFF) must happen from a single thread before app enumeration begins.
 */
class AppRegistry {
public:
    static AppRegistry& getInstance() {
        static AppRegistry instance;
        return instance;
    }

    /// Register an application entry
    void registerApp(const AppEntry& entry);

    /// Get the raw array of registered apps
    const AppEntry* getApps() const { return apps; }

    /// Get the number of registered apps
    size_t getAppCount() const { return appCount; }

    /// Registrations that did not fit. Anything but zero is a bug in the
    /// making: the app is simply not on the board.
    size_t getDroppedCount() const { return dropped_; }

    /// Find an app by name (returns nullptr if not found)
    const AppEntry* findApp(const char* name) const;

    /// Get the app with highest priority (for auto-selection)
    const AppEntry* getSelectedApp() const;

    /// Print all registered apps to stdout
    void listAllApps() const;

private:
    AppRegistry() = default;

    /* Twenty-four, because sixteen was reached. The apps that ship are
     * sixteen on their own now, and the launcher registers its own entries
     * (Power OFF) after them -- which registerApp() dropped without a word,
     * so the board simply had no way to be turned off. An AppEntry is a
     * handful of pointers; eight more cost less than that bug did. */
    static constexpr size_t MAX_APPS = 24;
    AppEntry apps[MAX_APPS];
    size_t appCount = 0;
    size_t dropped_ = 0;
};

} // namespace crosspad

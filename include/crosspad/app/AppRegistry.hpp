#pragma once

/**
 * @file AppRegistry.hpp
 * @brief Portable application registry singleton
 *
 * Stores registered AppEntry descriptors. Pure C++ — no platform dependencies.
 * Apps register via static constructors (AppRegistrar) or explicit registerApp() calls.
 */

#include "crosspad/app/AppEntry.hpp"
#include <cstddef>

namespace crosspad {

/**
 * @brief Singleton registry of all known applications
 *
 * Thread-safety note: registration happens during static initialization
 * (before main), so no mutex is needed. Runtime registration (e.g. Power OFF)
 * should happen from a single thread before app enumeration begins.
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

    /// Find an app by name (returns nullptr if not found)
    const AppEntry* findApp(const char* name) const;

    /// Get the app with highest priority (for auto-selection)
    const AppEntry* getSelectedApp() const;

    /// Print all registered apps to stdout
    void listAllApps() const;

private:
    AppRegistry() = default;

    static constexpr size_t MAX_APPS = 16;
    AppEntry apps[MAX_APPS];
    size_t appCount = 0;
};

} // namespace crosspad

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <functional>
#include <map>
#include <string>
#include <mutex>

namespace crosspad {

/// Callback function type for RGB color updates
typedef std::function<void()> RGBUpdateCallback;

/**
 * @brief RGB Callback Manager for handling external color update requests
 * Thread-safe via mutex protection
 */
class RGBCallbackManager {
public:
    /// Register a callback for RGB updates (thread-safe)
    void registerCallback(const std::string& name, RGBUpdateCallback callback);

    /// Unregister a callback (thread-safe)
    void unregisterCallback(const std::string& name);

    /// Trigger all registered callbacks (thread-safe)
    void triggerAllCallbacks();

    /// Trigger a specific callback by name (thread-safe)
    void triggerCallback(const std::string& name);

private:
    std::map<std::string, RGBUpdateCallback> callbacks;
    mutable std::mutex mutex;
};

} // namespace crosspad

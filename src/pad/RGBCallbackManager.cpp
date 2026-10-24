// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/pad/RGBCallbackManager.hpp"
#include <cstdio>

namespace crosspad {

void RGBCallbackManager::registerCallback(const std::string& name, RGBUpdateCallback callback) {
    std::lock_guard<std::mutex> lock(mutex);
    callbacks[name] = callback;
    printf("RGBCallbackManager: Registered callback '%s'\n", name.c_str());
}

void RGBCallbackManager::unregisterCallback(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = callbacks.find(name);
    if (it != callbacks.end()) {
        callbacks.erase(it);
        printf("RGBCallbackManager: Unregistered callback '%s'\n", name.c_str());
    }
}

void RGBCallbackManager::triggerAllCallbacks() {
    std::map<std::string, RGBUpdateCallback> callbacksCopy;
    {
        std::lock_guard<std::mutex> lock(mutex);
        callbacksCopy = callbacks;
    }

    for (const auto& pair : callbacksCopy) {
        try {
            pair.second();
        } catch (...) {
            printf("RGBCallbackManager: Error in callback '%s'\n", pair.first.c_str());
        }
    }
}

void RGBCallbackManager::triggerCallback(const std::string& name) {
    RGBUpdateCallback callback;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = callbacks.find(name);
        if (it != callbacks.end()) {
            callback = it->second;
            found = true;
        }
    }

    if (found) {
        try {
            callback();
        } catch (...) {
            printf("RGBCallbackManager: Error in callback '%s'\n", name.c_str());
        }
    }
}

} // namespace crosspad

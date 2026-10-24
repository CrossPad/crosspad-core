// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/app/AppRegistry.hpp"
#include <cstdio>
#include <cstring>

namespace crosspad {

void AppRegistry::registerApp(const AppEntry& entry) {
    if (appCount < MAX_APPS) {
        apps[appCount] = entry;
        appCount++;
        return;
    }
    /* Said out loud. Dropping this silently is how Power OFF disappeared
     * from a board that had grown to sixteen apps. */
    ++dropped_;
    printf("AppRegistry: no room for '%s' (%u registered, max %u)\n",
           entry.name ? entry.name : "?", (unsigned)appCount, (unsigned)MAX_APPS);
}

const AppEntry* AppRegistry::findApp(const char* name) const {
    for (size_t i = 0; i < appCount; i++) {
        if (strcmp(apps[i].name, name) == 0) {
            return &apps[i];
        }
    }
    return nullptr;
}

const AppEntry* AppRegistry::getSelectedApp() const {
    const AppEntry* selectedApp = nullptr;
    int highestPriority = -1;

    for (size_t i = 0; i < appCount; i++) {
        if (apps[i].priority > highestPriority) {
            highestPriority = apps[i].priority;
            selectedApp = &apps[i];
        } else if (apps[i].priority == highestPriority && selectedApp == nullptr) {
            selectedApp = &apps[i];
        }
    }

    return selectedApp;
}

void AppRegistry::listAllApps() const {
    printf("Registered applications (%zu):\n", appCount);
    for (size_t i = 0; i < appCount; i++) {
        printf("  %zu. %s\n", i, apps[i].name);
    }
}

} // namespace crosspad

// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/control/AppName.hpp"

#include <cctype>

namespace crosspad::control {

bool appNameMatches(const char* appName, const char* requested) {
    if (!appName || !requested) return false;
    for (; *appName && *requested; ++appName, ++requested) {
        if (std::tolower((unsigned char)*appName) != std::tolower((unsigned char)*requested)) return false;
    }
    return *appName == *requested;
}

} // namespace crosspad::control

// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/control/KitList.hpp"

#include "crosspad/platform/PlatformServices.hpp"

namespace crosspad::control {

bool kitIdLoadable(int id) {
    IKitManager* mgr = getKitManager();
    return mgr && id >= 0 && id < mgr->getKitCount();
}

} // namespace crosspad::control

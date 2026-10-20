// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/control/KitStatus.hpp"

#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/platform/PlatformServices.hpp"

namespace crosspad::control {

KitStatus readKitStatus(bool loading, int pending) {
    KitStatus st;
    st.loading = loading;
    st.pending = pending;
    IKitManager* mgr = getKitManager();
    if (!mgr) return st;
    st.count   = mgr->getKitCount();
    st.current = mgr->getCurrentKitId();
    if (const KitInfo* kit = mgr->getCurrentKit()) {
        st.name   = kit->name.c_str();
        st.parsed = kit->parsed;
    }
    return st;
}

} // namespace crosspad::control

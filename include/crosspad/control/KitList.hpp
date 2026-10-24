// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file KitList.hpp
 * @brief The kit listing and the load request's id check (board KIT_LIST /
 *        KIT_LOAD, simulator kit_list / kit_load), gathered once.
 */

#include "crosspad/kit/IKitManager.hpp"

namespace crosspad::control {

struct KitListRow {
    int         id      = 0;
    const char* name    = nullptr;   ///< the kit's own string; null where the manager has no kit at id
    bool        parsed  = false;
    /// Deleted while the board ran: the kit keeps its slot so no id shifts
    /// (IKitManager::removeKit), but has no path and is no longer a kit.
    bool        removed = false;
};

/// Every slot of @p mgr in id order, one row at a time to @p onRow.
template <class OnRow>
void readKitList(IKitManager& mgr, OnRow&& onRow) {
    const int count = mgr.getKitCount();
    for (int i = 0; i < count; ++i) {
        const KitInfo* kit = mgr.getKit(i);
        KitListRow row;
        row.id = i;
        if (kit) {
            row.name    = kit->name.c_str();
            row.parsed  = kit->parsed;
            row.removed = kit->path.empty();
        }
        onRow(row);
    }
}

/// Whether a load request names a kit: a manager, and an id it holds.
bool kitIdLoadable(int id);

} // namespace crosspad::control

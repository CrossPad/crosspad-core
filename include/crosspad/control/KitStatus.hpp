// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file KitStatus.hpp
 * @brief The kit verb (board KIT_STATUS, simulator kit_status), gathered once.
 *
 * crosspad/control/ is the state behind the device-control verbs both
 * platforms answer: each verb is read here into a plain struct, and each
 * platform only renders it in its own wire format -- CDC text on the board,
 * JSON on the simulator's control port. A field one wire format leaves out is
 * still read, so the two cannot disagree about what the state is.
 */

namespace crosspad::control {

struct KitStatus {
    int         count   = 0;        ///< kits the manager holds; 0 without a manager
    int         current = -1;       ///< current kit id; -1 without a manager
    /// The current kit's name, pointing into the kit manager's own copy: render
    /// it before anything can change the kit list. Null without a current kit.
    const char* name    = nullptr;
    bool        parsed  = false;    ///< the current kit's pads are parsed
    bool        loading = false;    ///< a kit load is in flight
    int         pending = -1;       ///< kit id waiting behind that load, -1 for none
};

/// The kit manager's side of the status. The kit loader is the sampler's, out
/// of core's sight, so its two facts come in from the caller.
KitStatus readKitStatus(bool loading, int pending);

} // namespace crosspad::control

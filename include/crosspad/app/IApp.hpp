// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace crosspad {

/**
 * @brief Abstract application interface for cross-platform app management.
 *
 * The name every application has, whatever the UI framework. An LVGL app's
 * lifecycle is ILvglApp / AppBase in crosspad-gui: the framework starts it
 * through its create function and tears it down through its destroy
 * function.
 */
class IApp {
public:
    virtual ~IApp() = default;

    /// Unique display name of this application.
    virtual const char* getName() const = 0;

    // ── Optional hooks ──────────────────────────────────────────────────
    /// Optional hook. Put set-up and clean-up in the app's create and
    /// destroy functions (crosspad-gui AppBase), which the framework calls.
    virtual void onAppInit() {}
    /// Optional hook. Put set-up and clean-up in the app's create and
    /// destroy functions (crosspad-gui AppBase), which the framework calls.
    virtual void onAppDeinit() {}
    /// Optional hook. Put set-up and clean-up in the app's create and
    /// destroy functions (crosspad-gui AppBase), which the framework calls.
    virtual void onAppUpdate() {}
    /// Optional hook. Put set-up and clean-up in the app's create and
    /// destroy functions (crosspad-gui AppBase), which the framework calls.
    virtual void onAppActivated() {}
};

} // namespace crosspad

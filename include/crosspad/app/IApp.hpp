// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace crosspad {

/**
 * @brief Abstract application interface for cross-platform app management.
 *
 * Defines the lifecycle callbacks that every application must support,
 * regardless of the UI framework (LVGL, Simple GUI, headless, etc.).
 */
class IApp {
public:
    virtual ~IApp() = default;

    /// Unique display name of this application.
    virtual const char* getName() const = 0;

    // ── Lifecycle callbacks ─────────────────────────────────────────────
    virtual void onAppInit() {}
    virtual void onAppDeinit() {}
    virtual void onAppUpdate() {}
    virtual void onAppActivated() {}
};

} // namespace crosspad

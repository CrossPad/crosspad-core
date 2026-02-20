#pragma once

#include "crosspad/app/IApp.hpp"
#include "crosspad/event/IEventBus.hpp"

namespace crosspad {

/**
 * @brief Cross-platform base class for application management.
 *
 * Provides the shared logic that every platform's AppManager needs:
 * - EventBus subscription forwarding to the active IApp
 * - update() loop delegation
 * - getCurrentAppName()
 * - switchToNextApp() / switchToPrevApp() with wrap-around
 *
 * Platform subclasses override the abstract methods to handle
 * LVGL vs Simple GUI specifics (app creation, switching, etc.).
 */
class AppManagerBase {
public:
    virtual ~AppManagerBase() = default;

    // ── Event forwarding ────────────────────────────────────────────────

    /// Subscribe to NoteOn/Off and PadPressed/Released, forward to currentApp_.
    void initEventForwarding(IEventBus& eventBus);

    /// Unsubscribe the four event handlers.
    void deinitEventForwarding(IEventBus& eventBus);

    // ── Update loop ─────────────────────────────────────────────────────

    /// Calls currentApp_->onAppUpdate() if an app is active.
    void update();

    // ── Queries ─────────────────────────────────────────────────────────

    IApp* getCurrentApp() const { return currentApp_; }
    const char* getCurrentAppName() const;
    virtual int getCurrentAppIndex() const = 0;

    // ── Switching ───────────────────────────────────────────────────────

    virtual bool switchToApp(int index) = 0;
    virtual bool switchToApp(const char* name) = 0;

    /// Switch to next app (wraps around). Uses getAppCount() + getCurrentAppIndex().
    bool switchToNextApp();

    /// Switch to previous app (wraps around).
    bool switchToPrevApp();

protected:
    /// Subclasses set this when switching apps.
    IApp* currentApp_ = nullptr;

    /// Subclasses report the total number of available apps.
    virtual size_t getAppCount() const = 0;

private:
    SubscriptionHandle noteOnSub_ = nullptr;
    SubscriptionHandle noteOffSub_ = nullptr;
    SubscriptionHandle padPressedSub_ = nullptr;
    SubscriptionHandle padReleasedSub_ = nullptr;
};

} // namespace crosspad

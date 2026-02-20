#pragma once

#include <cstdint>

namespace crosspad {

/**
 * @brief Abstract application interface for cross-platform app management.
 *
 * Defines the business-logic callbacks that every application must support,
 * regardless of the UI framework (LVGL, Simple GUI, headless, etc.).
 * AppManagerBase routes EventBus events through these methods.
 */
class IApp {
public:
    virtual ~IApp() = default;

    /// Unique display name of this application.
    virtual const char* getName() const = 0;

    // ── Business-logic callbacks (empty defaults) ───────────────────────
    virtual void onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {}
    virtual void onNoteOff(uint8_t channel, uint8_t note) {}
    virtual void onPadPressed(uint8_t padIdx, bool isOn, uint8_t velocity) {}
    virtual void onPadReleased(uint8_t padIdx) {}

    // ── Lifecycle callbacks ─────────────────────────────────────────────
    virtual void onAppInit() {}
    virtual void onAppDeinit() {}
    virtual void onAppUpdate() {}
    virtual void onAppActivated() {}
};

} // namespace crosspad

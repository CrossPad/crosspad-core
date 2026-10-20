// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

namespace crosspad {

/// What Settings → Programming shows about a co-processor the platform can
/// reflash by itself from an image it carries.
struct CoprocFwInfo {
    char running[32];   ///< e.g. "1.2 (proto 0x000E)"; "" = link down
    char bundled[32];   ///< the image the platform ships
    bool due;           ///< bundled is newer or speaks another protocol
    bool canUpdate;     ///< an update can be started right now
};

/**
 * @brief Platform callback interface for the shared Settings UI.
 *
 * The Settings UI in crosspad-gui calls these methods when it needs
 * platform-specific actions (persisting changes, returning to home screen).
 * Each platform registers its implementation before opening Settings.
 */
class ISettingsUI {
public:
    virtual ~ISettingsUI() = default;

    /// Persist current CrosspadSettings to platform storage (NVS, JSON file, etc.)
    virtual void saveSettings() = 0;

    /// Navigate back to the home/launcher screen
    virtual void exitToHome() = 0;

    /// Get platform name for the System info page (e.g., "PC Simulator", "ESP32-S3")
    virtual const char* getPlatformName() { return "CrossPad"; }

    /// Get platform-specific info string (e.g., profile dir path). nullptr = hide row.
    virtual const char* getPlatformInfo() { return nullptr; }

    /// Request STM32 co-processor to enter DFU mode. Default no-op for platforms
    /// without an STM32 (PC sim). Implementers send the DFU sysex command.
    virtual void enterStm32Dfu() {}

    /// Co-processor firmware the platform can flash itself. false = nothing to
    /// show (no co-processor, or no bundled image) and the rows are hidden.
    virtual bool getCoprocFwInfo(CoprocFwInfo& /*out*/) { return false; }

    /// Start the platform's own update flow (its confirm dialog, progress and
    /// failure guidance) — the UI only asks, it never flashes.
    virtual void requestCoprocFwUpdate() {}

    /// Rows for the Info page: firmware/library revisions, co-processor identity,
    /// serial number — whatever the platform can report. The UI walks 0..count-1
    /// and shows label/value pairs, so platforms stay free to add entries without
    /// touching the shared settings UI. Strings must stay valid until the next
    /// call (a static/member buffer is fine).
    virtual int getInfoRowCount() { return 0; }
    virtual bool getInfoRow(int /*idx*/, const char** /*label*/, const char** /*value*/) { return false; }

    /// Directory for settings backup files (e.g. "/sdcard"). nullptr = the
    /// platform has no removable storage, and the UI hides the backup actions.
    virtual const char* getBackupDir() { return nullptr; }
};

/// Set the active settings UI callback (called by platform before opening Settings)
void setSettingsUI(ISettingsUI* ui);

/// Get the active settings UI callback (used by crosspad-gui Settings components)
ISettingsUI* getSettingsUI();

} // namespace crosspad

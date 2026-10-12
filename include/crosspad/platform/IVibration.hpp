#pragma once
#include <cstdint>
#include "crosspad/platform/VibrationPreset.hpp"

namespace crosspad {

class VibrationSettings;

/**
 * @brief Abstract vibration/haptics interface
 *
 * Implementations:
 *   - Drv2605Driver (crosspad-platform-idf): DRV2605/LRA driver using II2cDevice
 *   - NullVibration (core): no-op stub for platforms without haptics
 *
 * The interface does NOT manage initialization, I2C setup, or background tasks —
 * those remain in the platform layer.
 */
class IVibration {
public:
    virtual ~IVibration() = default;

    /// Whether vibration hardware is present and initialized
    virtual bool isAvailable() const = 0;

    /// Trigger a haptic preset (async, queued to background task)
    virtual void vibrate(VibrationPreset preset) = 0;

    /// Stop any active vibration
    virtual void stop() = 0;

    /// Enter audio-to-vibration mode (motor follows audio input signal)
    virtual void startAudioToVibe() = 0;

    /// Exit audio-to-vibration mode
    virtual void stopAudioToVibe() = 0;

    /// Push updated settings to the driver
    virtual void applySettings(const VibrationSettings& settings) = 0;
};

/// Platform-injected singleton (same pattern as getClock())
IVibration& getVibration();

} // namespace crosspad

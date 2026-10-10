// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/platform/IVibration.hpp"
#include "crosspad/settings/VibrationSettings.hpp"

namespace crosspad {

/**
 * @brief No-op IVibration stub for platforms without haptic hardware.
 *
 * Used as the default singleton until a real driver is registered,
 * and as the permanent implementation on platforms that lack vibration
 * (e.g. PC simulator).
 */
class NullVibration : public IVibration {
public:
    bool isAvailable() const override { return false; }
    void vibrate(VibrationPreset) override {}
    void stop() override {}
    void startAudioToVibe() override {}
    void stopAudioToVibe() override {}
    void applySettings(const VibrationSettings&) override {}
};

} // namespace crosspad

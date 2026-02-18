#pragma once
#include <cstdint>

// PadManager lives outside crosspad-core (platform-specific),
// forward-declared here so IPadLogicHandler can reference it.
class PadManager;

namespace crosspad {

/**
 * @brief Interface for application-specific pad logic handlers
 *
 * Each application (sequencer, sampler, etc.) can implement this interface
 * to define custom behavior when pads are pressed, released, or receive pressure.
 */
class IPadLogicHandler {
public:
    virtual ~IPadLogicHandler() = default;

    /// Called when this handler becomes the active pad logic
    virtual void onActivate(PadManager& padManager) = 0;

    /// Called when this handler is deactivated
    virtual void onDeactivate(PadManager& padManager) = 0;

    /// Called when a pad is pressed (padIdx 0-15, velocity 0-127)
    virtual void onPadPress(PadManager& padManager, uint8_t padIdx, uint8_t velocity) = 0;

    /// Called when a pad is released (padIdx 0-15)
    virtual void onPadRelease(PadManager& padManager, uint8_t padIdx) = 0;

    /// Called when pad pressure changes — aftertouch (padIdx 0-15, pressure 0-127)
    virtual void onPadPressure(PadManager& padManager, uint8_t padIdx, uint8_t pressure) = 0;
};

} // namespace crosspad

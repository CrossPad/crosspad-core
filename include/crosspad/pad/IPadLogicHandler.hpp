#pragma once
#include <cstdint>
#include "crosspad/pad/PadPerformance.hpp"

namespace crosspad {

// Forward declaration — PadManager is now in crosspad-core
class PadManager;

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

    /// A finger is on @p padIdx and its velocity is still being measured
    /// (@p touching), or it left without a hit (!@p touching) -- from a pad
    /// controller that says so (STM PAD_TOUCH). A handler that plays sound
    /// can get the note ready here and only set its level in onPadPress(),
    /// which follows as always. Optional: nothing is lost by ignoring it.
    virtual void onPadTouch(PadManager& padManager, uint8_t padIdx, bool touching) {
        (void)padManager; (void)padIdx; (void)touching;
    }

    /// Called when pad pressure changes — aftertouch (padIdx 0-15, pressure 0-127)
    virtual void onPadPressure(PadManager& padManager, uint8_t padIdx, uint8_t pressure) = 0;

    /// Called when the press moves across the pad — left/right balance of the
    /// two cells under it, -127 (left) .. +127 (right), 0 centred. Streams
    /// while the pad is held, on change only. Optional: a handler that plays
    /// notes has no use for it.
    virtual void onPadPan(PadManager& padManager, uint8_t padIdx, int8_t pan) {
        (void)padManager; (void)padIdx; (void)pan;
    }

    /// What a press of @p padIdx would sound as now, for the loop recorder to
    /// keep with the hit; @p velocity becomes the one that would sound. A
    /// handler with no performance mode keeps the default: plain, as struck.
    virtual PadPerformance performance(uint8_t padIdx, uint8_t& velocity) const {
        (void)padIdx; (void)velocity;
        return {};
    }
};

} // namespace crosspad

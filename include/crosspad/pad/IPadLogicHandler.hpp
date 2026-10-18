// SPDX-License-Identifier: GPL-3.0-or-later
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
 *
 * Threads. The pad callbacks (onPadPress, onPadRelease, onPadTouch,
 * onPadPressure, onPadPan, and the performance() query) come from whatever feeds the pads -- the pad task, MIDI input,
 * the UI, test injection -- and may run on several threads at once.
 * onActivate() and onDeactivate() run on the thread that changes the active
 * logic (an app's start and teardown: the UI thread), outside PadManager's
 * lock; pad callbacks may already arrive while onActivate() runs.
 *
 * Deactivation guarantee: once onDeactivate() is called, and after it
 * returns, no pad callback of this handler runs or is in flight. PadManager
 * stops dispatching to the handler, then waits for the callbacks other
 * threads are inside before it calls onDeactivate(), so a press racing an
 * app's teardown cannot start a note or voice after it was cleaned up.
 *
 * Re-entrancy rules for a pad callback:
 * - it may call PadManager's state, note, colour and LED methods, and
 *   handlePad*()/notifyPad*() (a nested dispatch);
 * - it must not change the active logic (setActivePadLogic, push/popPadLogic,
 *   unregisterPadLogic, setBasePadLogic) -- post that to the UI thread. Done
 *   from a callback it cannot wait for the others, and the guarantee above
 *   does not hold for that change;
 * - it must not block on anything the thread changing the logic may hold:
 *   that thread waits for the callback. Above all never on the LVGL lock --
 *   post GUI work with IGuiPlatform::postToGui(), which never waits for it.
 * onActivate()/onDeactivate() must not change the active logic either.
 */
class IPadLogicHandler {
public:
    /// The range onPadPan() delivers. The STM streams each held pad's
    /// left/right balance as a 7-bit controller value centred on
    /// kPanWireCentre; the pan is that value minus the centre.
    static constexpr int8_t kPanMin = -64;        ///< all of the press on the left cell
    static constexpr int8_t kPanCentre = 0;
    static constexpr int8_t kPanMax = 63;         ///< all of the press on the right cell
    static constexpr uint8_t kPanWireCentre = 64; ///< the 7-bit value of a centred press

    /// The pan of a 7-bit wire value (0 left .. 64 centred .. 127 right).
    static constexpr int8_t panFromWire(uint8_t wire) {
        return static_cast<int8_t>(static_cast<int>(wire & 0x7F) - kPanWireCentre);
    }

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
    /// two cells under it, kPanMin (-64, left) .. kPanMax (+63, right),
    /// kPanCentre (0) centred. Streams while the pad is held, on change only.
    /// Optional: a handler that plays notes has no use for it.
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

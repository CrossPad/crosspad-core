// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/instrument/InstrumentPadLogic.hpp"
#include "crosspad/pad/PadManager.hpp"

#include <atomic>
#include <cstdint>

namespace crosspad {

/// What a Pitch seq track's notes play through while the song plays them.
///
/// A seq track is the sequencer's scene, and the sequencer plays a kit track
/// by pressing pads -- which reach whatever pad logic the screen on top has.
/// A Pitch track's notes come on a route of their own (AudioSong::seqRoute()
/// 2) and go straight to the instrument whatever the screen, beside the kit
/// tracks on route 0: the platform's sequencer sink calls playPitch().
/// Arrange keeps the instrument loaded (it owns the seq tracks).
struct SeqTrackSound {
    /// The instrument's pads; ready() once the host's bank is playing.
    InstrumentPadLogic pitch;
    /// Before core 2.6 a Pitch track shared route 0 with the pads' kit, and
    /// this sent route 0 to the instrument while set. Kept for sinks and apps
    /// built against that (crosspad-pc's Arrange); this firmware's Arrange
    /// never sets it, its Pitch notes come on route 2.
    std::atomic<bool> routePitch{false};

    /// The sequencer's note on the Pitch route.
    void playPitch(PadManager& pm, uint8_t pad, uint8_t velocity, bool on)
    {
        if (on) pitch.onPadPress(pm, pad, velocity);
        else    pitch.onPadRelease(pm, pad);
    }

    /// A route-0 note while routePitch is set (see there): true when it went
    /// to the instrument and must not reach the pads as well.
    bool route(PadManager& pm, uint8_t pad, uint8_t velocity, bool on)
    {
        if (!routePitch.load(std::memory_order_acquire)) return false;
        playPitch(pm, pad, velocity, on);
        return true;
    }
};

/// The one router. Header-only, as arrangeFocus() is: an inline function's
/// local static is one object across every translation unit.
inline SeqTrackSound& seqTrackSound()
{
    static SeqTrackSound sound;
    return sound;
}

} // namespace crosspad

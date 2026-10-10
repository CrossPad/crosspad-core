// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/instrument/InstrumentPadLogic.hpp"
#include "crosspad/pad/PadManager.hpp"

#include <atomic>
#include <cstdint>

namespace crosspad {

/// What a Pitch seq track's notes play through while the song plays them.
///
/// A seq track is the sequencer's scene, and the sequencer plays it by
/// pressing pads -- which reach whatever pad logic the screen on top has. In
/// the seq view that is the instrument (the Sequencer pushes it); anywhere
/// else it is the kit, and with the pitched engine holding the arena the kit
/// is silent: a Pitch track played from Arrange made no sound at all, and
/// came alive only in EDIT.
///
/// So the song's notes for a Pitch track go to the instrument whatever the
/// screen: the platform's sequencer sink asks route() first. Arrange keeps the
/// instrument loaded and says when to route (it owns the seq tracks).
struct SeqTrackSound {
    /// The instrument's pads; ready() once the host's bank is playing.
    InstrumentPadLogic pitch;
    /// The song is playing (or bouncing) a Pitch seq track.
    std::atomic<bool> routePitch{false};

    /// The sequencer's note: true when it went to the track's instrument and
    /// must not reach the pads as well.
    bool route(PadManager& pm, uint8_t pad, uint8_t velocity, bool on)
    {
        if (!routePitch.load(std::memory_order_acquire)) return false;
        if (on) pitch.onPadPress(pm, pad, velocity);
        else    pitch.onPadRelease(pm, pad);
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

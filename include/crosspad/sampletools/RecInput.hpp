// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace crosspad {

/// What a take records.
enum class RecInput : uint8_t {
    Mic,    ///< the built-in microphones
    Line,   ///< the line-in jack
    Pads,   ///< what the pads play: a resample, digital, no codec in the way
    Usb,    ///< what the computer plays into the board over USB audio, before the mixer's fader
    /** A Pitch Play instrument on the pads.
     *
     * The tap is the same digital bus as @c Pads -- a pitched bank renders
     * into it where a kit's streamer would -- and what makes this a source of
     * its own is the engine behind it: selecting it borrows the engine
     * (IInstrumentHost) for the instrument Pitch Play has, and the pads sound
     * it with Pitch Play's key, scale, transpose and chord
     * (InstrumentPlayState). Not a choice of its own, deliberately: one place
     * sets the instrument, and that place is Pitch Play. */
    Pitch,
    /// The built-in microphones and the line-in at once, summed: a voice and
    /// an instrument into one take. Both codecs' ADCs are held for it.
    MicLine,
};

} // namespace crosspad

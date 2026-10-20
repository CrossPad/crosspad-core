// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace crosspad {

/// A kit beside the one the pads play: what a song's second kit track plays
/// through (AudioSong::seqRoute() 1).
///
/// The platform holds it in a sample engine of its own, and nothing reaches
/// it through the pads' logic -- that is the first kit's -- so it is played by
/// note: the sequencer's sink for the track's notes, SecondKitPadLogic for the
/// fingers while the track is armed. While it holds a kit the two kits share
/// the voices the board can stream, five each.
class ISecondKit {
public:
    enum class State : uint8_t { Idle, Loading, Ready, Failed };

    virtual ~ISecondKit() = default;

    /// Hold @p kitPath (a kit.json as the kit list names it), or nothing for
    /// an empty one. Returns at once; asking for what it holds, or is
    /// loading, does nothing -- so a keeper may ask every tick.
    virtual void hold(const std::string& kitPath) = 0;
    virtual State state() const = 0;
    /// The kit it holds, or is loading; empty with none.
    virtual std::string kit() const = 0;

    /// A pad of it, from any thread but the render's. A release matters only
    /// to a pad on HOLD, as with the pads' kit.
    virtual void noteOn(uint8_t pad, uint8_t velocity) = 0;
    virtual void noteOff(uint8_t pad) = 0;

    /// Its loudest sample since the last ask, 0..1 a side.
    virtual void takeLevel(float& left, float& right) = 0;
};

} // namespace crosspad

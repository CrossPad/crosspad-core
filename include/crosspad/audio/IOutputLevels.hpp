// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace crosspad {

/// The board's physical audio outputs -- the built-in speakers, the jacks --
/// each at its own level, set in the hardware after the DAC: turning one down
/// costs no resolution and takes the hiss down with it. One mixer output can
/// feed more than one of them (the speakers and a jack beside them hear the
/// same stream), and here they still have a level each.
class IOutputLevels {
public:
    virtual ~IOutputLevels() = default;

    /// How many outputs the board has; they are numbered 0 .. outputs()-1.
    virtual int         outputs() const = 0;
    /// What the board calls output @p o ("Speakers", "Out 1/2").
    virtual const char* name(int o) const = 0;
    /// The mixer output whose signal it carries, as AudioMixerEngine names it.
    virtual const char* mixerOutput(int o) const = 0;
    /// 0..100.
    virtual int         level(int o) const = 0;
    /// Clamped to 0..100; kept across boots.
    virtual bool        setLevel(int o, int level) = 0;
    /// Silenced, as the hardware can: a speaker amp switched off, a jack at
    /// its codec's lowest step. The level is kept for when it comes back.
    virtual bool        muted(int o) const = 0;
    /// Kept across boots.
    virtual bool        setMuted(int o, bool muted) = 0;
};

} // namespace crosspad

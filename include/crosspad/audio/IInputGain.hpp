// SPDX-License-Identifier: MIT
#pragma once

namespace crosspad {

/// The gain of the board's audio inputs, before anything else sees them: the
/// mixer's input channels, a recording, the USB microphone. A preamp, not a
/// fader -- where the platform can, the first part of it is analog.
class IInputGain {
public:
    virtual ~IInputGain() = default;

    virtual int         inputs() const = 0;
    /// The mixer channel input @p i feeds (as AudioMixerEngine names it).
    virtual const char* channelName(int i) const = 0;
    virtual float       gainDb(int i) const = 0;
    /// Rounded to stepDb() and clamped to 0..maxDb(); kept across boots.
    virtual bool        setGainDb(int i, float db) = 0;
    virtual float       maxDb() const = 0;
    virtual float       stepDb() const = 0;
};

} // namespace crosspad

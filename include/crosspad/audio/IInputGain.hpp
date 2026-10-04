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

    /* Where an input listens, when the board gives it a choice.
     *
     * A board can wire two sockets to one converter, and then only one of
     * them can be heard at a time -- CrossPad does exactly that with its two
     * line-in jacks. Such an input reports two or more sources and the UI
     * offers a switch; everything else reports none and nothing changes.
     * Default: no choice, which is what every other platform has. */
    virtual int         sources(int i) const { (void)i; return 0; }
    virtual const char* sourceName(int i, int s) const { (void)i; (void)s; return ""; }
    virtual int         source(int i) const { (void)i; return 0; }
    virtual bool        setSource(int i, int s) { (void)i; (void)s; return false; }
};

} // namespace crosspad

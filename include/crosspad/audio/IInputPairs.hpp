// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace crosspad {

/// Which of the board's inputs reach its converters. A board with more inputs
/// than ADCs connects some of them at a time; every combination it can make is
/// a pair here, named for the inputs it connects. An input left out keeps its
/// mixer channel, sends and gain -- it is silent until a pair connects it.
class IInputPairs {
public:
    virtual ~IInputPairs() = default;

    virtual int         pairs() const = 0;
    /// Short enough for a button: "Mic + 1/2".
    virtual const char* pairName(int i) const = 0;
    virtual int         pair() const = 0;
    /// Kept across boots.
    virtual bool        setPair(int i) = 0;
    /// Whether the mixer channel named @p channelName (as AudioMixerEngine
    /// names it) is connected in the current pair.
    virtual bool        isConnected(const char* channelName) const = 0;
    /// The input converter @p codec hears in the current pair ("Mic",
    /// "Line 1/2", "Line 3/4"), what a take of that converter records; null
    /// where the board does not say.
    virtual const char* inputOn(int codec) const { (void)codec; return nullptr; }
};

} // namespace crosspad

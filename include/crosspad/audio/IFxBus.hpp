// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace crosspad {

class IAudioNode;

/// What can feed the effects bus.
enum class FxSource : uint8_t {
    Line12,   ///< the line inputs 1/2
    Line34,   ///< the line inputs 3/4
    Mic,      ///< the built-in microphones
    Usb,      ///< what the computer plays over USB audio
    Count
};

/// Where the effects bus can return.
enum class FxOutput : uint8_t {
    Out12,    ///< the phones (and the speakers beside them)
    Out34,    ///< the line out
    Usb,      ///< the computer, over USB audio
    Count
};

/// One effects bus as an insert: sources are sent to it, an app's processor
/// runs over their sum in the same render pass, and what it makes is
/// returned to the outputs picked -- the board's whole path while the
/// processor is attached. A performance FX app owns it: it attaches its
/// processor, picks the sources, their send levels and the outputs.
///
/// While attached, every source with a send is taken off its own routes to
/// the outputs, so the processor carries the dry signal too (its dry/wet is
/// the app's); detach() puts those routes back. The board picks which two
/// of the three analog inputs its ADCs hear from the sends
/// (sourceAvailable() is false for one left out) and holds their ADCs on
/// them. Null where the platform has no mixer.
class IFxBus {
public:
    virtual ~IFxBus() = default;

    /// Whether this board has @p s at all.
    virtual bool hasSource(FxSource s) const = 0;
    /// Whether @p s is heard now: USB only in the USB audio profile, an
    /// analog input only while an ADC is on it.
    virtual bool sourceAvailable(FxSource s) const = 0;
    /// @p level 0..1 of @p s into the bus; 0 = not sent.
    virtual void  setSend(FxSource s, float level) = 0;
    virtual float send(FxSource s) const = 0;
    /// The send before the source's fader (its pan only) rather than after.
    virtual void setPreFader(bool pre) = 0;

    virtual bool hasOutput(FxOutput o) const = 0;
    virtual void setReturn(FxOutput o, bool on) = 0;
    virtual bool returnOn(FxOutput o) const = 0;

    /// The processor: an IAudioNode whose process() runs in place over the
    /// bus every block, sound or not. It must live as long as the firmware:
    /// a render pass may still be finishing on it after detach().
    virtual void attach(IAudioNode* processor) = 0;
    virtual void detach() = 0;
    /// What the processor made in the last block, the peak a side, 0..1.
    virtual void level(float& left, float& right) const = 0;
};

} // namespace crosspad

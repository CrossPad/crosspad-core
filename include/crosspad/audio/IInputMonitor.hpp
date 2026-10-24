// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/sampletools/ISampleTools.hpp"

namespace crosspad {

/// Hearing an input live: its mixer channel sent to every output a person
/// hears (speakers, jacks) or to none -- the Mixer's MONITOR set to "all" or
/// "off", for an app that only knows the input by what it records (an audio
/// track's RecInput). The platform maps an input to its channels; MicLine is
/// both of its own. The Mixer screen shows the change, and the desk keeps it
/// as the Mixer's own does.
class IInputMonitor {
public:
    virtual ~IInputMonitor() = default;
    /// Whether @p in has a channel of its own to send: false for what is
    /// heard anyway (the pads) and for an input this board does not have.
    virtual bool canMonitor(RecInput in) const = 0;
    /// Whether @p in is heard: any of its channels sent to a heard output.
    virtual bool monitoring(RecInput in) const = 0;
    /// Send @p in to every heard output (a send dragged to nothing comes back
    /// at unity), or take it off all of them. False when it cannot be.
    virtual bool setMonitoring(RecInput in, bool on) = 0;
};

} // namespace crosspad

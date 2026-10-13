// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace crosspad {

/// Level and pan of one pad in whichever engine plays the pads now -- the
/// sample player or the pitched instrument. An app that mixes pads (Pad
/// Mixer) goes through PlatformServices::padMix and never needs to know
/// which engine that is, or that there are two.
class IPadMix {
public:
    virtual ~IPadMix() = default;
    /// In the kit's own units -- Pad::volume and Pad::pan, as kit.json holds
    /// them -- so a caller hands over what it edited without converting.
    /// Applies to the next hit and to voices already sounding.
    virtual void setPadMix(uint8_t pad, int volume, int pan) = 0;
};

} // namespace crosspad

// SPDX-License-Identifier: MIT
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
    /// @p volume 0..1, @p pan -1 (left) .. +1 (right). Applies to the next
    /// hit and to voices already sounding.
    virtual void setPadMix(uint8_t pad, float volume, float pan) = 0;
};

} // namespace crosspad

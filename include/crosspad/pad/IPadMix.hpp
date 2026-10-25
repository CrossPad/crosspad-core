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

    /// Meters, for a mixer that shows them: on while it is open, since the
    /// engine then measures every voice it renders.
    virtual void setMetering(bool on) { (void)on; }
    /// Each pad's peak since the last call into @p peaks[16] -- linear, 1.0
    /// = full scale, after the pad's volume and pan (PadMeters). False where
    /// the engine has no meters; @p peaks is then left alone.
    virtual bool takePeaks(float* peaks) { (void)peaks; return false; }
};

} // namespace crosspad

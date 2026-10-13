// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/sampler/SamplerKit.hpp"

namespace crosspad {

void SamplerKit::panGains(float balance, float& l, float& r)
{
    if (balance < -1.0f) balance = -1.0f;
    if (balance >  1.0f) balance =  1.0f;
    /* Balance, as the mixer's channels: unity at the centre, and turning only
     * takes level from the other side. The equal-power law this replaced put
     * every centred pad -- nearly all of them -- 3 dB under the same sample
     * auditioned in the Slicer, played by the engine before this one, or
     * through any channel of the mixer (09.10.2026). */
    l = balance > 0.0f ? 1.0f - balance : 1.0f;
    r = balance < 0.0f ? 1.0f + balance : 1.0f;
}

} // namespace crosspad

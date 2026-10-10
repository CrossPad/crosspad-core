// SPDX-License-Identifier: MIT
#include "crosspad/sampler/SamplerKit.hpp"

#include <cmath>

namespace crosspad {

void SamplerKit::panGains(float balance, float& l, float& r)
{
    if (balance < -1.0f) balance = -1.0f;
    if (balance >  1.0f) balance =  1.0f;
    /* Equal power: the two gains are the cosine and sine of a quarter turn, so
     * their squares sum to one and a pad swept across the image keeps its
     * loudness. A linear pan dips 3 dB in the middle, which on a kit where
     * one pad is panned and its neighbour is not reads as the panned one
     * being quieter rather than as it being to one side. */
    const float a = (balance + 1.0f) * 0.25f * 3.14159265358979f;   // 0 .. pi/2
    l = std::cos(a);
    r = std::sin(a);
}

} // namespace crosspad

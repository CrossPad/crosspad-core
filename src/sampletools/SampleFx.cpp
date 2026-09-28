// SPDX-License-Identifier: MIT
#include "crosspad/sampletools/SampleFx.hpp"

#include "crosspad/dsp/DspMath.hpp"

#include <algorithm>
#include <cmath>

namespace crosspad {

void Biquad::set(Kind kind, float hz, uint32_t rate)
{
    if (!rate || hz <= 0.0f || hz >= 0.45f * (float)rate) { on_ = false; return; }
    const float w0 = 2.0f * (float)kPi * hz / (float)rate;
    const float c = std::cos(w0);
    const float alpha = std::sin(w0) / (2.0f * 0.70710678f);   // Butterworth
    const float a0 = 1.0f + alpha;
    if (kind == Kind::LowPass) {
        b0_ = (1.0f - c) / 2.0f / a0;
        b1_ = (1.0f - c) / a0;
        b2_ = b0_;
    } else {
        b0_ = (1.0f + c) / 2.0f / a0;
        b1_ = -(1.0f + c) / a0;
        b2_ = b0_;
    }
    a1_ = -2.0f * c / a0;
    a2_ = (1.0f - alpha) / a0;
    on_ = true;
    reset();
}

void SampleFilters::set(const SampleFx& fx, uint32_t rate)
{
    if (fx.hpHz) hp_.set(Biquad::Kind::HighPass, fx.hpHz, rate); else hp_.off();
    if (fx.lpHz) lp_.set(Biquad::Kind::LowPass, fx.lpHz, rate); else lp_.off();
}

void SampleFilters::process(int16_t* io, uint32_t n)
{
    if (!on()) return;
    for (uint32_t k = 0; k < n * 2; ++k) {
        const int ch = (int)(k & 1);
        float v = (float)io[k];
        if (hp_.on()) v = hp_.process(v, ch);
        if (lp_.on()) v = lp_.process(v, ch);
        io[k] = (int16_t)std::max(-32768.0f, std::min(32767.0f, std::round(v)));
    }
}

} // namespace crosspad

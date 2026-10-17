// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/dsp/AudioKernels.hpp"

namespace crosspad::dsp {

void mac(float* dst, const float* src, float g, uint32_t n)
{
#if CROSSPAD_DSP_PIE
    if (quadAligned(n, dst, src)) { pie::mac(dst, src, g, n); return; }
#endif
    fast::mac(dst, src, g, n);
}

void macPanned(float* out, const float* mono, uint32_t frames, float gl, float gr)
{
#if CROSSPAD_DSP_PIE
    if (quadAligned(frames, out, mono)) { pie::macPanned(out, mono, frames, gl, gr); return; }
#endif
    fast::macPanned(out, mono, frames, gl, gr);
}

void scaleStereo(float* buf, uint32_t frames, float gl, float gr)
{
#if CROSSPAD_DSP_PIE
    if (quadAligned(frames * 2, buf)) { pie::scaleStereo(buf, frames, gl, gr); return; }
#endif
    fast::scaleStereo(buf, frames, gl, gr);
}

void peakStereo(const float* buf, uint32_t frames, float& pL, float& pR)
{
    /* No PIE path: on the board (16.10.2026) its four-float loads read a
     * channel's block no faster than the portable loop (565 vs 595 cycles
     * alone) and the render's peaks took 39 us against 32 with them. */
    fast::peakStereo(buf, frames, pL, pR);
}

void clampPeakStereo(float* buf, uint32_t frames, float& pL, float& pR)
{
    peakStereo(buf, frames, pL, pR);
    if (pL > 1.0f || pR > 1.0f) fast::clampPeakStereo(buf, frames, pL, pR);
}

void biquadStereo(float* io, uint32_t frames, const Biquad& c, float* z1, float* z2)
{
    /* No PIE path: the section is FPU-bound and the wide loads saved nothing
     * on the board (1182 vs 1151 cycles a block, the four-float version
     * spilling a register); with it and the PIE peaks the output stage took
     * 120 us a block against 110. */
    fast::biquadStereo(io, frames, c, z1, z2);
}

} // namespace crosspad::dsp

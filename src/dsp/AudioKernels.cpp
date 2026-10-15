// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/dsp/AudioKernels.hpp"

namespace crosspad::dsp {

void mac(float* dst, const float* src, float g, uint32_t n)
{
    switch (kernelMode()) {
    case KernelMode::Reference: ref::mac(dst, src, g, n); return;
#if CROSSPAD_DSP_PIE
    case KernelMode::Pie:
        if (quadAligned(n, dst, src)) { pie::mac(dst, src, g, n); return; }
        break;
#endif
    default: break;
    }
    fast::mac(dst, src, g, n);
}

void macPanned(float* out, const float* mono, uint32_t frames, float gl, float gr, float* tmp)
{
    switch (kernelMode()) {
    case KernelMode::Reference: ref::macPanned(mono, out, frames, gl, gr, tmp); return;
#if CROSSPAD_DSP_PIE
    case KernelMode::Pie:
        if (quadAligned(frames, out, mono)) { pie::macPanned(out, mono, frames, gl, gr); return; }
        break;
#endif
    default: break;
    }
    fast::macPanned(out, mono, frames, gl, gr);
}

void scaleStereo(float* buf, uint32_t frames, float gl, float gr)
{
    switch (kernelMode()) {
    case KernelMode::Reference: ref::scaleStereo(buf, frames, gl, gr); return;
#if CROSSPAD_DSP_PIE
    case KernelMode::Pie:
        if (quadAligned(frames * 2, buf)) { pie::scaleStereo(buf, frames, gl, gr); return; }
        break;
#endif
    default: break;
    }
    fast::scaleStereo(buf, frames, gl, gr);
}

void peakStereo(const float* buf, uint32_t frames, float& pL, float& pR)
{
    switch (kernelMode()) {
    case KernelMode::Reference: ref::peakStereo(buf, frames, pL, pR); return;
#if CROSSPAD_DSP_PIE
    case KernelMode::Pie:
        if (quadAligned(frames * 2, buf)) { pie::peakStereo(buf, frames, pL, pR); return; }
        break;
#endif
    default: break;
    }
    fast::peakStereo(buf, frames, pL, pR);
}

void clampPeakStereo(float* buf, uint32_t frames, float& pL, float& pR)
{
    if (kernelMode() == KernelMode::Reference) { ref::clampPeakStereo(buf, frames, pL, pR); return; }
    peakStereo(buf, frames, pL, pR);
    if (pL > 1.0f || pR > 1.0f) ref::clampPeakStereo(buf, frames, pL, pR);
}

void biquadStereo(float* io, uint32_t frames, const Biquad& c, float* z1, float* z2)
{
    switch (kernelMode()) {
    case KernelMode::Reference: ref::biquadStereo(io, frames, c, z1, z2); return;
#if CROSSPAD_DSP_PIE
    case KernelMode::Pie:
        if (quadAligned(frames * 2, io)) { pie::biquadStereo(io, frames, c, z1, z2); return; }
        break;
#endif
    default: break;
    }
    fast::biquadStereo(io, frames, c, z1, z2);
}

} // namespace crosspad::dsp

// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/dsp/AudioKernels.hpp"

#include <atomic>

namespace crosspad::dsp {

namespace {

using MacFn       = decltype(AudioKernelTable::mac);
using MacPannedFn = decltype(AudioKernelTable::macPanned);
using ScaleFn     = decltype(AudioKernelTable::scaleStereo);
using PeakFn      = decltype(AudioKernelTable::peakStereo);
using BiquadFn    = decltype(AudioKernelTable::biquadStereo);
using ToInt16Fn   = decltype(AudioKernelTable::floatToInt16);

void portableFloatToInt16(const float* src, int16_t* dst, uint32_t n)
{
    fast::floatToInt16With(src, dst, n, fast::truncSat, fast::clampS16);
}

/* Relaxed: a table is registered once, before audio starts; the loads only
 * have to be whole pointers. */
std::atomic<MacFn>       s_mac{fast::mac};
std::atomic<MacPannedFn> s_macPanned{fast::macPanned};
std::atomic<ScaleFn>     s_scaleStereo{fast::scaleStereo};
std::atomic<PeakFn>      s_peakStereo{fast::peakStereo};
std::atomic<BiquadFn>    s_biquadStereo{fast::biquadStereo};
std::atomic<ToInt16Fn>   s_floatToInt16{portableFloatToInt16};

/* Fn comes from the slot alone: MSVC counts a parameter's __restrict in the
 * function type, so &fast::mac and the table's MacFn would not deduce alike. */
template <typename Fn>
void install(std::atomic<Fn>& slot, typename std::atomic<Fn>::value_type platform,
             typename std::atomic<Fn>::value_type portable)
{
    slot.store(platform ? platform : portable, std::memory_order_relaxed);
}

}  // namespace

void setAudioKernels(const AudioKernelTable* table)
{
    const AudioKernelTable none;
    const AudioKernelTable& t = table ? *table : none;
    install(s_mac, t.mac, &fast::mac);
    install(s_macPanned, t.macPanned, &fast::macPanned);
    install(s_scaleStereo, t.scaleStereo, &fast::scaleStereo);
    install(s_peakStereo, t.peakStereo, &fast::peakStereo);
    install(s_biquadStereo, t.biquadStereo, &fast::biquadStereo);
    install(s_floatToInt16, t.floatToInt16, &portableFloatToInt16);
}

void mac(float* dst, const float* src, float g, uint32_t n)
{
    s_mac.load(std::memory_order_relaxed)(dst, src, g, n);
}

void macPanned(float* out, const float* mono, uint32_t frames, float gl, float gr)
{
    s_macPanned.load(std::memory_order_relaxed)(out, mono, frames, gl, gr);
}

void scaleStereo(float* buf, uint32_t frames, float gl, float gr)
{
    s_scaleStereo.load(std::memory_order_relaxed)(buf, frames, gl, gr);
}

void peakStereo(const float* buf, uint32_t frames, float& pL, float& pR)
{
    s_peakStereo.load(std::memory_order_relaxed)(buf, frames, pL, pR);
}

void clampPeakStereo(float* buf, uint32_t frames, float& pL, float& pR)
{
    peakStereo(buf, frames, pL, pR);
    if (pL > 1.0f || pR > 1.0f) fast::clampPeakStereo(buf, frames, pL, pR);
}

void biquadStereo(float* io, uint32_t frames, const Biquad& c, float* z1, float* z2)
{
    s_biquadStereo.load(std::memory_order_relaxed)(io, frames, c, z1, z2);
}

void floatToInt16(const float* src, int16_t* dst, uint32_t n)
{
    s_floatToInt16.load(std::memory_order_relaxed)(src, dst, n);
}

} // namespace crosspad::dsp

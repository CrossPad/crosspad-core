// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/dsp/PitchDetector.hpp"
#include "crosspad/dsp/DspMath.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace crosspad {

namespace {

constexpr uint32_t kN = kPitchWindow;          // analysis window / FFT size
constexpr uint32_t kStep = kPitchWindow / 4;   // RMS search hop
constexpr float kMinHz = 40.0f;                // lowest fundamental considered

/// Weighted log-domain harmonic product. Plain (unweighted) HPS ties on a pure
/// sine — k0, k0/2 and k0/3 all score "one loud bin times two noise bins" —
/// and the argmax then lands an octave or a twelfth low. Decaying weights
/// break the tie towards the true fundamental while still rejecting the
/// harmonic of a rich tone.
constexpr float kHarmonicWeight[] = { 1.0f, 0.5f, 0.25f };
constexpr uint32_t kHarmonics = sizeof(kHarmonicWeight) / sizeof(kHarmonicWeight[0]);

/// Attack transient skipped before the loudest-window search, in milliseconds.
/// A pick or a hammer is the loudest part of most recordings and the least
/// tonal part of any of them.
constexpr uint32_t kAttackSkipMs = 20;

/// Magnitudes below this fraction of the peak are treated as the peak's floor,
/// so log() of an empty bin cannot dominate the harmonic sum.
constexpr float kMagFloorRatio = 1e-5f;

/// Half-widths of the successive DTFT refinement passes, in FFT bins.
constexpr double kRefineSteps[] = { 0.25, 0.05, 0.01 };

/// Bins either side of the HPS argmax searched for the true magnitude peak.
constexpr uint32_t kPeakSnapBins = 2;

/// Magnitude of the Hann-windowed frame's DTFT at a fractional bin position.
/// Used to refine the FFT-grid peak: the parabolic fit on the coarse grid
/// carries a ~0.02 bin bias for a Hann window, which is 3 cents at 100 Hz —
/// too close to the 5 cent budget to rely on.
double dtftMag(const float* frame, double bin) {
    const double w = -2.0 * kPi * bin / static_cast<double>(kN);
    const double cs = std::cos(w), sn = std::sin(w);
    double pr = 1.0, pi = 0.0;   // rotating phasor, exp(w*i*n)
    double sr = 0.0, si = 0.0;
    for (uint32_t n = 0; n < kN; ++n) {
        const double x = frame[n];
        sr += x * pr;
        si += x * pi;
        const double nr = pr * cs - pi * sn;
        pi = pr * sn + pi * cs;
        pr = nr;
    }
    return std::sqrt(sr * sr + si * si);
}

/// Parabolic vertex of three log-magnitudes sampled at -h, 0, +h.
double parabolicOffset(double lm, double l0, double lp, double h) {
    const double denom = lm - 2.0 * l0 + lp;
    if (denom >= 0.0) return 0.0;   // not a maximum
    double d = 0.5 * (lm - lp) / denom * h;
    if (d < -h) d = -h;
    if (d >  h) d =  h;
    return d;
}

} // namespace

bool detectPitch(IFft& fft, const int16_t* pcm, uint32_t count, uint32_t sampleRate,
                 PitchResult& out, float* scratch) {
    out = PitchResult{};
    if (!pcm || count == 0 || sampleRate == 0) return false;

    std::vector<float> owned;
    if (!scratch) {
        owned.resize(kPitchScratchFloats);
        scratch = owned.data();
    }
    const uint32_t half = kN / 2;
    float* frame = scratch;
    float* buf = frame + kN;
    float* mag = buf + 2 * kN;
    std::fill(scratch, scratch + kPitchScratchFloats, 0.0f);

    // ── pick the loudest window, skipping the attack ──
    const uint32_t skip = sampleRate * kAttackSkipMs / 1000u;
    uint32_t best = 0;
    if (count > kN) {
        const uint32_t last = count - kN;
        uint32_t start = (skip < last) ? skip : 0;
        best = start;
        double bestEnergy = -1.0;
        for (uint32_t p = start; p <= last; p += kStep) {
            double e = 0.0;
            for (uint32_t i = 0; i < kN; ++i) {
                const double s = pcm[p + i];
                e += s * s;
            }
            if (e > bestEnergy) { bestEnergy = e; best = p; }
        }
        if (bestEnergy <= 0.0) return false;
    }

    // ── Hann-windowed frame, zero-padded when the input is short ──
    const uint32_t avail = (count > best) ? (count - best) : 0;
    const uint32_t take = (avail < kN) ? avail : kN;
    double energy = 0.0;
    for (uint32_t i = 0; i < take; ++i) {
        const float w = 0.5f - 0.5f * std::cos(2.0f * static_cast<float>(kPi)
                                             * static_cast<float>(i) / static_cast<float>(kN));
        frame[i] = pcm[best + i] * w;
        energy += static_cast<double>(pcm[best + i]) * pcm[best + i];
    }
    if (energy <= 0.0) return false;

    for (uint32_t i = 0; i < kN; ++i) buf[2 * i] = frame[i];
    if (!fft.forward(buf, kN)) return false;

    float maxMag = 0.0f;
    double magSum = 0.0;
    for (uint32_t k = 0; k <= half; ++k) {
        mag[k] = std::sqrt(buf[2 * k] * buf[2 * k] + buf[2 * k + 1] * buf[2 * k + 1]);
        if (k > 0) { magSum += mag[k]; if (mag[k] > maxMag) maxMag = mag[k]; }
    }
    if (maxMag <= 0.0f) return false;

    // ── weighted harmonic product spectrum ──
    const float floorMag = maxMag * kMagFloorRatio;
    auto logMag = [&](uint32_t k) {
        const float m = (mag[k] > floorMag) ? mag[k] : floorMag;
        return std::log(m);
    };
    // Search from kMinHz up to the highest fundamental whose kHarmonics'th
    // harmonic is still inside the spectrum.
    uint32_t binLo = static_cast<uint32_t>(kMinHz * static_cast<float>(kN)
                                         / static_cast<float>(sampleRate));
    if (binLo < 2) binLo = 2;
    const uint32_t binHi = half / kHarmonics;
    if (binLo >= binHi) return false;

    uint32_t peak = binLo;
    float bestScore = -1e30f;
    for (uint32_t k = binLo; k < binHi; ++k) {
        float score = 0.0f;
        for (uint32_t h = 0; h < kHarmonics; ++h) score += kHarmonicWeight[h] * logMag(k * (h + 1));
        if (score > bestScore) { bestScore = score; peak = k; }
    }

    // Snap to the nearest local magnitude maximum — the HPS argmax can sit a
    // bin off the true peak because the harmonic terms drag it.
    uint32_t lo = (peak > kPeakSnapBins) ? peak - kPeakSnapBins : 1;
    uint32_t hi = (peak + kPeakSnapBins < half) ? peak + kPeakSnapBins : half - 1;
    uint32_t kp = lo;
    for (uint32_t k = lo; k <= hi; ++k) if (mag[k] > mag[kp]) kp = k;
    if (kp == 0 || kp >= half) return false;

    // ── coarse parabolic vertex on the FFT grid, then a DTFT refinement ──
    double bin = kp + parabolicOffset(logMag(kp - 1), logMag(kp), logMag(kp + 1), 1.0);
    for (double h : kRefineSteps) {
        const double lm = std::log(dtftMag(frame, bin - h) + 1e-30);
        const double l0 = std::log(dtftMag(frame, bin)     + 1e-30);
        const double lp = std::log(dtftMag(frame, bin + h) + 1e-30);
        bin += parabolicOffset(lm, l0, lp, h);
    }
    if (bin <= 0.0) return false;

    out.hz = static_cast<float>(bin * sampleRate / static_cast<double>(kN));
    if (out.hz <= 0.0f) return false;
    out.midi = 69.0f + 12.0f * std::log2(out.hz / 440.0f);
    const double meanMag = magSum / half;
    out.confidence = (meanMag > 0.0) ? static_cast<float>(mag[kp] / meanMag) : 0.0f;
    return true;
}

} // namespace crosspad

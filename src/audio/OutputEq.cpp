// SPDX-License-Identifier: MIT
#include "crosspad/audio/OutputEq.hpp"

#include <cmath>
#include <new>

namespace crosspad {

namespace {
/* Robert Bristow-Johnson's cookbook, normalised by a0. A band whose
 * frequency is at or past Nyquist, or whose Q is nonsense, is left out
 * rather than allowed to blow up. */
constexpr float kMinQ = 0.20f, kMaxQ = 16.0f;
} // namespace

EqChain::~EqChain()
{
    delete st_.load(std::memory_order_relaxed);
}

EqChain::State* EqChain::state()
{
    State* st = st_.load(std::memory_order_acquire);
    if (st) return st;
    st = new (std::nothrow) State();
    if (!st) return nullptr;
    st_.store(st, std::memory_order_release);
    return st;
}

EqSettings EqChain::settings() const
{
    const State* st = st_.load(std::memory_order_acquire);
    return st ? st->want : EqSettings{};
}

void EqChain::setRate(uint32_t rate)
{
    if (!rate || rate == rate_) return;
    rate_ = rate;
    /* The coefficients are the rate's. Nothing to redo for an output nobody
     * has EQ'd -- and nothing is taken for it either. */
    if (const State* st = st_.load(std::memory_order_acquire)) set(st->want);
}

void EqChain::reset()
{
    State* st = st_.load(std::memory_order_acquire);
    if (!st) return;
    for (int b = 0; b < kEqBands; ++b) {
        st->z1[b][0] = st->z1[b][1] = 0.0f;
        st->z2[b][0] = st->z2[b][1] = 0.0f;
    }
}

void EqChain::build(Set& out) const
{
    out.n = 0;
    const State* st = st_.load(std::memory_order_acquire);
    if (!st || !st->want.on) return;
    for (const EqBand& band : st->want.band) {
        if (band.kind == EqKind::Off) continue;
        const bool pass = band.kind == EqKind::HighPass || band.kind == EqKind::LowPass;
        if (!pass && band.gainDb == 0.0f) continue;          // a flat band is no band
        const float hz = band.hz;
        if (hz <= 10.0f || hz >= (float)rate_ * 0.48f) continue;
        float q = band.q;
        if (!(q > kMinQ)) q = kMinQ;
        if (q > kMaxQ) q = kMaxQ;

        const float w0 = 6.28318530718f * hz / (float)rate_;
        const float cw = std::cos(w0), sw = std::sin(w0);
        const float alpha = sw / (2.0f * q);
        float b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

        switch (band.kind) {
        case EqKind::Peak: {
            const float A = std::pow(10.0f, band.gainDb / 40.0f);
            b0 = 1.0f + alpha * A;
            b1 = -2.0f * cw;
            b2 = 1.0f - alpha * A;
            a0 = 1.0f + alpha / A;
            a1 = -2.0f * cw;
            a2 = 1.0f - alpha / A;
            break;
        }
        case EqKind::LowShelf: {
            const float A = std::pow(10.0f, band.gainDb / 40.0f);
            const float sa = 2.0f * std::sqrt(A) * alpha;
            b0 = A * ((A + 1.0f) - (A - 1.0f) * cw + sa);
            b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cw);
            b2 = A * ((A + 1.0f) - (A - 1.0f) * cw - sa);
            a0 = (A + 1.0f) + (A - 1.0f) * cw + sa;
            a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cw);
            a2 = (A + 1.0f) + (A - 1.0f) * cw - sa;
            break;
        }
        case EqKind::HighShelf: {
            const float A = std::pow(10.0f, band.gainDb / 40.0f);
            const float sa = 2.0f * std::sqrt(A) * alpha;
            b0 = A * ((A + 1.0f) + (A - 1.0f) * cw + sa);
            b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw);
            b2 = A * ((A + 1.0f) + (A - 1.0f) * cw - sa);
            a0 = (A + 1.0f) - (A - 1.0f) * cw + sa;
            a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cw);
            a2 = (A + 1.0f) - (A - 1.0f) * cw - sa;
            break;
        }
        case EqKind::HighPass:
            b0 = (1.0f + cw) * 0.5f;
            b1 = -(1.0f + cw);
            b2 = (1.0f + cw) * 0.5f;
            a0 = 1.0f + alpha;
            a1 = -2.0f * cw;
            a2 = 1.0f - alpha;
            break;
        case EqKind::LowPass:
            b0 = (1.0f - cw) * 0.5f;
            b1 = 1.0f - cw;
            b2 = (1.0f - cw) * 0.5f;
            a0 = 1.0f + alpha;
            a1 = -2.0f * cw;
            a2 = 1.0f - alpha;
            break;
        default:
            continue;
        }
        if (a0 == 0.0f) continue;
        Section& s = out.s[out.n++];
        s.b0 = b0 / a0;
        s.b1 = b1 / a0;
        s.b2 = b2 / a0;
        s.a1 = a1 / a0;
        s.a2 = a2 / a0;
        if (out.n >= kEqBands) break;
    }
}

void EqChain::set(const EqSettings& s)
{
    State* st = state();
    if (!st) return;
    st->want = s;
    if (!s.on || !s.any()) {
        /* Nothing to run. The sets are left alone (an EQ switched off and on
         * again is the common case) but the render is told to skip. */
        count_.store(0, std::memory_order_release);
        return;
    }
    /* The half the render is not reading, then one store to hand it over. */
    const uint8_t next = (uint8_t)(1u - (live_.load(std::memory_order_relaxed) & 1u));
    build(st->sets[next]);
    const uint8_t n = st->sets[next].n;
    live_.store(next, std::memory_order_release);
    count_.store(n, std::memory_order_release);
}

void EqChain::process(float* io, uint32_t frames)
{
    if (!io || !frames) return;
    if (!count_.load(std::memory_order_acquire)) return;
    State* st = st_.load(std::memory_order_acquire);
    if (!st) return;
    const Set* set = &st->sets[live_.load(std::memory_order_acquire) & 1u];
    const uint8_t n = set->n;
    if (!n) return;
    for (uint8_t b = 0; b < n; ++b) {
        /* Copied out of the set once a band: the coefficients may well be in
         * a pool the render reads slowly, and the inner loop must not. */
        const Section s = set->s[b];
        for (int ch = 0; ch < 2; ++ch) {
            float z1 = st->z1[b][ch], z2 = st->z2[b][ch];
            for (uint32_t f = 0; f < frames; ++f) {
                const float x = io[f * 2 + ch];
                const float y = s.b0 * x + z1;
                z1 = s.b1 * x - s.a1 * y + z2;
                z2 = s.b2 * x - s.a2 * y;
                io[f * 2 + ch] = y;
            }
            st->z1[b][ch] = z1;
            st->z2[b][ch] = z2;
        }
    }
}

} // namespace crosspad

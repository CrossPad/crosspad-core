// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/dsp/AudioKernels.hpp"

#include <atomic>
#include <cstdint>

namespace crosspad {

/// What one band of an output's EQ does. Off leaves the band out of the chain
/// altogether, so an EQ nobody has set costs the render nothing.
enum class EqKind : uint8_t { Off, LowShelf, Peak, HighShelf, HighPass, LowPass };

struct EqBand {
    EqKind kind   = EqKind::Off;
    float  hz     = 1000.0f;
    float  gainDb = 0.0f;      ///< shelves and peaks; the passes ignore it
    float  q      = 0.70f;     ///< 0.3 .. 8; a shelf reads it as its slope

    bool operator==(const EqBand& o) const
    {
        return kind == o.kind && hz == o.hz && gainDb == o.gainDb && q == o.q;
    }
    bool operator!=(const EqBand& o) const { return !(*this == o); }
};

/// Four bands is what evening out a speaker takes: the two ends, and two
/// things in the middle. More than that is a design tool, not a correction.
constexpr int kEqBands = 4;

struct EqSettings {
    EqBand band[kEqBands];
    bool   on = false;         ///< the whole chain, one switch

    /// Any band that would actually do something.
    bool any() const
    {
        for (const auto& b : band) {
            if (b.kind == EqKind::Off) continue;
            if (b.kind == EqKind::HighPass || b.kind == EqKind::LowPass) return true;
            if (b.gainDb != 0.0f) return true;
        }
        return false;
    }
    bool operator==(const EqSettings& o) const
    {
        if (on != o.on) return false;
        for (int i = 0; i < kEqBands; ++i) if (band[i] != o.band[i]) return false;
        return true;
    }
};

/// An output's EQ as the render runs it: up to four RBJ sections over
/// interleaved stereo float, in place.
///
/// **Coefficients are not worked out in the render.** A sine and a cosine per
/// band is not something a block of audio should wait for, so whoever sets
/// the bands computes them -- into the half of a double buffer the render is
/// not reading -- and publishes the half with one atomic store. A block in
/// flight finishes on the coefficients it started with.
///
/// The sections' state stays where it is across a change. A coefficient swap
/// under a sounding signal is a click at worst, which is what every desk's EQ
/// does, and the alternative (zeroing the state) is a louder one.
class EqChain {
public:
    EqChain() = default;
    ~EqChain();
    EqChain(const EqChain&) = delete;
    EqChain& operator=(const EqChain&) = delete;

    void setRate(uint32_t rate);
    /// Not from the render thread.
    void set(const EqSettings& s);
    EqSettings settings() const;
    /// Whether the render has anything to do.
    bool on() const { return count_.load(std::memory_order_acquire) != 0; }
    /// @p frames interleaved stereo frames, filtered in place. Render thread.
    void process(float* io, uint32_t frames);
    /// Forget the sections' state (an output that fell silent).
    void reset();

private:
    using Section = dsp::Biquad;
    struct Set { Section s[kEqBands]; uint8_t n = 0; };

    /* Everything an EQ needs, taken the first time one is asked for and not
     * before: two coefficient sets, the sections' state, and what was asked
     * for.
     *
     * Why none of it lives in the object: this board boots with a few hundred
     * bytes of internal RAM to spare. The startup reserve of 64 kB for DMA is
     * the first thing to fail when that runs out -- "Could not reserve
     * internal/DMA pool", abort(), before app_main -- and two sets of four
     * sections per output was enough to cross the line twice over. An output
     * nobody has EQ'd costs a pointer.
     */
    struct State {
        Set        sets[2];
        float      z1[kEqBands][2] = {};
        float      z2[kEqBands][2] = {};
        EqSettings want;
    };

    /// Takes the state if it is not there yet; null when memory says no.
    State* state();

    void build(Set& out) const;

    std::atomic<State*>  st_{nullptr};
    std::atomic<uint8_t> live_{0};
    std::atomic<uint8_t> count_{0};
    uint32_t rate_ = 48000;
};

} // namespace crosspad

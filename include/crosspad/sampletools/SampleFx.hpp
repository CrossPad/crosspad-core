// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace crosspad {

/// What a sample is played with on its pad: backwards, transposed
/// (varispeed) and filtered, high pass then low pass, 12 dB per octave each.
/// The sample player can do none of it, so a pad with any of it plays a copy
/// rendered once (renderedSamplePath()); an audition applies the same live.
///
/// @c keepLength transposes without the tempo: the sample stretched by the
/// pitch's ratio (TimeStretch::run), then read back at it (repitch), so it
/// lasts as long as before -- what a clip on a song's bars needs. Rendered
/// only; an audition plays it varispeed.
struct SampleFx {
    int      semitones = 0;    ///< -12..+12 (SliceMath::pitchRatio)
    uint16_t hpHz = 0;         ///< high pass cutoff; 0 = off
    uint16_t lpHz = 0;         ///< low pass cutoff; 0 = off
    bool     reverse = false;  ///< played from its end to its start
    bool     keepLength = false; ///< with semitones: the length kept, not varispeed

    bool any() const { return semitones || hpHz || lpHz || reverse; }
    bool operator==(const SampleFx& o) const
    {
        return semitones == o.semitones && hpHz == o.hpHz && lpHz == o.lpHz && reverse == o.reverse &&
               keepLength == o.keepLength;
    }
};

/// The cutoffs a knob steps through: the third-octave series, 20 Hz to 20 kHz.
constexpr uint16_t kFxCutoffs[] = {20, 25, 32, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630,
                                   800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000,
                                   12500, 16000, 20000};
constexpr int kFxCutoffCount = sizeof(kFxCutoffs) / sizeof(kFxCutoffs[0]);

/// One second-order section (RBJ), stereo, run in place on interleaved int16.
class Biquad {
public:
    enum class Kind : uint8_t { LowPass, HighPass };
    /// @p hz at @p rate; a cutoff at or past the top of the band is "off".
    void set(Kind kind, float hz, uint32_t rate);
    void off() { on_ = false; }
    void reset() { z1_[0] = z1_[1] = z2_[0] = z2_[1] = 0.0f; }
    bool on() const { return on_; }
    float process(float x, int ch)
    {
        const float y = b0_ * x + z1_[ch];
        z1_[ch] = b1_ * x - a1_ * y + z2_[ch];
        z2_[ch] = b2_ * x - a2_ * y;
        return y;
    }

private:
    bool  on_ = false;
    float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    float z1_[2] = {}, z2_[2] = {};
};

/// A sample's filters, set once, then fed its frames in order.
class SampleFilters {
public:
    void set(const SampleFx& fx, uint32_t rate);
    bool on() const { return hp_.on() || lp_.on(); }
    void reset() { hp_.reset(); lp_.reset(); }
    /// @p n stereo frames, filtered in place.
    void process(int16_t* io, uint32_t n);

private:
    Biquad hp_, lp_;
};

} // namespace crosspad

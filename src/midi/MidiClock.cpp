// SPDX-License-Identifier: MIT
#include "crosspad/midi/MidiClock.hpp"

#include <cmath>
#include <cstdlib>

namespace crosspad {

MidiClock::MidiClock(float bpm, int64_t nowUs) : bpm_(clampBpm(bpm))
{
    restartGenerator(nowUs);
}

float MidiClock::clampBpm(float bpm)
{
    if (!(bpm >= kMinBpm)) return kMinBpm;   // NaN lands here too
    if (bpm > kMaxBpm) return kMaxBpm;
    return bpm;
}

double MidiClock::periodUs() const
{
    return 60e6 / (double(kPpqn) * double(bpm_));
}

void MidiClock::restartGenerator(int64_t nowUs)
{
    genT0_ = nowUs;
    genN_  = 0;
}

void MidiClock::markSourceHost(int64_t nowUs)
{
    lastHostUs_ = nowUs;
    if (source_ != Source::Host) {
        source_  = Source::Host;
        pending_ = true;
    }
}

void MidiClock::hostTick(int64_t nowUs)
{
    ++ticksIn_;
    if (count_ > 0) {
        const int64_t gap = nowUs - lastHostUs_;
        lastGapUs_ = (int32_t)gap;
        if (gap <= 0 || gap > kRestartGapUs) {
            count_ = 0;
            head_  = 0;
        }
    }
    ticks_[head_] = nowUs;
    head_ = (head_ + 1) % kWindow;
    if (count_ < kWindow) ++count_;
    markSourceHost(nowUs);

    if (count_ < kMinEstimate) return;
    /* Least-squares slope of tick time against tick index: the interval that
     * best explains the whole window, not just its two ends. Times are taken
     * relative to the oldest tick so the doubles keep their precision. */
    const uint32_t oldest = (head_ + kWindow - count_) % kWindow;
    const double n = double(count_);
    const double meanIdx = (n - 1.0) / 2.0;
    double meanT = 0.0;
    for (uint32_t i = 0; i < count_; ++i) meanT += double(ticks_[(oldest + i) % kWindow] - ticks_[oldest]);
    meanT /= n;
    double sxy = 0.0, sxx = 0.0;
    for (uint32_t i = 0; i < count_; ++i) {
        const double dx = double(i) - meanIdx;
        sxy += dx * (double(ticks_[(oldest + i) % kWindow] - ticks_[oldest]) - meanT);
        sxx += dx * dx;
    }
    const double interval = sxy / sxx;
    if (count_ == kWindow) {
        const int32_t dev = (int32_t)std::llabs((long long)lastGapUs_ - std::llround(interval));
        if (dev > maxJitterUs_) maxJitterUs_ = dev;
    }
    bpm_ = clampBpm(float(60e6 / (double(kPpqn) * interval)));
    if (std::fabs(bpm_ - publishedBpm_) > kPublishDeltaBpm) pending_ = true;
}

void MidiClock::hostStart(int64_t nowUs)
{
    running_ = true;
    markSourceHost(nowUs);
}

void MidiClock::hostContinue(int64_t nowUs)
{
    running_ = true;
    markSourceHost(nowUs);
}

void MidiClock::hostStop(int64_t nowUs)
{
    running_ = false;
    markSourceHost(nowUs);
}

bool MidiClock::poll(int64_t nowUs)
{
    if (source_ != Source::Host || nowUs - lastHostUs_ <= kHostTimeoutUs) return false;
    source_  = Source::Internal;
    count_   = 0;
    head_    = 0;
    pending_ = true;
    restartGenerator(nowUs);
    return true;
}

void MidiClock::setBpm(float bpm, int64_t nowUs)
{
    (void)nowUs;
    if (source_ == Source::Host) return;
    const float b = clampBpm(bpm);
    if (std::fabs(b - bpm_) < 1e-3f) return;
    genT0_ = nextTickDue();
    genN_  = 0;
    bpm_   = b;
    if (std::fabs(bpm_ - publishedBpm_) > kPublishDeltaBpm) pending_ = true;
}

int64_t MidiClock::nextTickDue() const
{
    return genT0_ + (int64_t)std::llround(double(genN_) * periodUs());
}

void MidiClock::internalTickSent(int64_t nowUs)
{
    const int64_t late = nowUs - nextTickDue();
    ++ticksOut_;
    if (late > (int64_t)periodUs()) {   // stalled: restart, never burst
        restartGenerator(nowUs);
        genN_ = 1;
        return;
    }
    if (late > maxLateUs_) maxLateUs_ = (int32_t)late;
    ++genN_;
}

bool MidiClock::takeTempoChange(float& bpm, Source& source)
{
    if (source_ != publishedSource_) pending_ = true;
    if (!pending_) return false;
    pending_         = false;
    publishedBpm_    = bpm_;
    publishedSource_ = source_;
    bpm    = bpm_;
    source = source_;
    return true;
}

void MidiClock::resetStats()
{
    lastGapUs_   = 0;
    maxJitterUs_ = 0;
    maxLateUs_   = 0;
}

} // namespace crosspad

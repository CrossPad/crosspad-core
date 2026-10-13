// SPDX-License-Identifier: GPL-3.0-or-later
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

void MidiClock::resetWindow()
{
    count_ = 0;
    head_  = 0;
    sumT_  = 0;
    sumIT_ = 0;
}

void MidiClock::hostTick(int64_t nowUs)
{
    ++ticksIn_;
    if (count_ > 0) {
        const int64_t gap = nowUs - lastTickUs_;
        lastGapUs_ = (int32_t)gap;
        if (gap <= 0 || gap > kRestartGapUs) resetWindow();
    }
    if (count_ < kWindow) {
        sumT_  += nowUs;
        sumIT_ += int64_t(count_) * nowUs;
        ++count_;
    } else {
        /* Slide: the oldest leaves, every other tick moves down one place. */
        const int64_t oldest = ticks_[head_];
        const int64_t rest   = sumT_ - oldest;
        sumIT_ = sumIT_ - rest + int64_t(kWindow - 1) * nowUs;
        sumT_  = rest + nowUs;
    }
    ticks_[head_] = nowUs;
    head_ = (head_ + 1) % kWindow;
    lastTickUs_ = nowUs;
    tickSinceStart_ = true;
    markSourceHost(nowUs);

    if (count_ < kMinEstimate) return;
    const int64_t n    = count_;
    const int64_t si   = n * (n - 1) / 2;
    const int64_t sii  = (n - 1) * n * (2 * n - 1) / 6;
    const double  interval = double(n * sumIT_ - si * sumT_) / double(n * sii - si * si);
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
    tickSinceStart_ = false;
    resetWindow();              // a new take: its tempo owes nothing to the last
    markSourceHost(nowUs);
}

void MidiClock::hostContinue(int64_t nowUs)
{
    running_ = true;
    tickSinceStart_ = false;
    markSourceHost(nowUs);
}

void MidiClock::hostStop(int64_t nowUs)
{
    running_ = false;
    markSourceHost(nowUs);
}

bool MidiClock::poll(int64_t nowUs)
{
    const int64_t timeout = (running_ && !tickSinceStart_) ? kCountInTimeoutUs : kHostTimeoutUs;
    if (source_ != Source::Host || nowUs - lastHostUs_ <= timeout) return false;
    source_  = Source::Internal;
    resetWindow();
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
    if (late > maxLateUs_) maxLateUs_ = (int32_t)late;
    if (late > (int64_t)periodUs()) {   // stalled: restart, never burst
        ++restarts_;
        restartGenerator(nowUs);
        genN_ = 1;
        return;
    }
    ++genN_;
}

void MidiClock::internalStart(int64_t nowUs)
{
    if (source_ == Source::Host) return;
    running_ = true;
    restartGenerator(nowUs);
}

void MidiClock::internalStop(int64_t nowUs)
{
    (void)nowUs;
    if (source_ == Source::Host) return;
    running_ = false;
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

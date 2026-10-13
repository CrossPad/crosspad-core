// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace crosspad {

/// One MIDI clock for the whole device. Follows a host's 24-PPQN clock while
/// it arrives, and generates the clock from the device's own tempo when the
/// host is silent. Not thread-safe: the platform serialises every call.
/// Time is microseconds on any monotonic clock.
class MidiClock {
public:
    enum class Source : uint8_t { Internal, Host };

    static constexpr uint32_t kPpqn            = 24;
    static constexpr int64_t  kHostTimeoutUs   = 1000000;
    /// After a Start, how long the host may stay silent before its first tick
    /// (a count-in) without losing the clock.
    static constexpr int64_t  kCountInTimeoutUs = 8000000;
    /// A longer gap between two host ticks means the host stopped and played
    /// again: the tempo window starts over instead of averaging across it.
    static constexpr int64_t  kRestartGapUs    = 250000;
    static constexpr float    kPublishDeltaBpm = 0.05f;
    static constexpr float    kMinBpm          = 20.0f;
    static constexpr float    kMaxBpm          = 300.0f;

    explicit MidiClock(float bpm = 120.0f, int64_t nowUs = 0);

    void hostTick(int64_t nowUs);
    void hostStart(int64_t nowUs);
    void hostContinue(int64_t nowUs);
    void hostStop(int64_t nowUs);

    /// True on the call that hands the clock back to the internal source.
    bool poll(int64_t nowUs);

    /// The device's own tempo (UI, kit). Ignored while the host drives.
    void setBpm(float bpm, int64_t nowUs);

    int64_t nextTickDue() const;
    void    internalTickSent(int64_t nowUs);
    void    tickForwarded() { ++ticksOut_; }
    /// The device's own click started / stopped (internal source only):
    /// tick 0 is due at `nowUs`, so the first F8 lands on the click's beat.
    void    internalStart(int64_t nowUs);
    void    internalStop(int64_t nowUs);

    Source source() const  { return source_; }
    float  bpm() const     { return bpm_; }
    bool   running() const { return running_; }

    /// A tempo or source change worth telling the UI about, reported once.
    bool takeTempoChange(float& bpm, Source& source);

    uint32_t ticksIn() const     { return ticksIn_; }
    uint32_t ticksOut() const    { return ticksOut_; }
    int32_t  lastGapUs() const   { return lastGapUs_; }
    int32_t  maxJitterUs() const { return maxJitterUs_; }
    int32_t  maxLateUs() const   { return maxLateUs_; }
    /// Times the generator stalled for more than a period and restarted.
    uint32_t restarts() const    { return restarts_; }
    void     resetStats();

private:
    static float clampBpm(float bpm);
    double periodUs() const;
    void   restartGenerator(int64_t nowUs);
    void   markSourceHost(int64_t nowUs);

    /// Four beats. The tempo is the least-squares slope over the window: with
    /// ±1 ms of USB jitter on each tick a one-beat mean is off by up to 0.6 %
    /// at 174 BPM, the four-beat slope by ~0.05 % (3 sigma).
    static constexpr uint32_t kWindow = 4 * kPpqn + 1;
    /// Ticks needed before the first estimate: half a beat, refined as it fills.
    static constexpr uint32_t kMinEstimate = kPpqn / 2 + 1;

    void resetWindow();

    int64_t  ticks_[kWindow] = {};
    uint32_t count_ = 0;
    uint32_t head_  = 0;
    /* Running sums for the least-squares fit, O(1) per tick and exact in
     * integers: sum of t, and of i*t with i the tick's place in the window. */
    int64_t  sumT_  = 0;
    int64_t  sumIT_ = 0;
    int64_t  lastHostUs_ = 0;    // any host byte: keeps the host the source
    int64_t  lastTickUs_ = 0;    // host ticks only: the restart gap and jitter
    bool     tickSinceStart_ = false;

    Source source_  = Source::Internal;
    float  bpm_;
    bool   running_ = false;

    int64_t  genT0_ = 0;
    uint64_t genN_  = 0;

    bool   pending_ = true;
    float  publishedBpm_ = 0.0f;
    Source publishedSource_ = Source::Internal;

    uint32_t ticksIn_ = 0;
    uint32_t ticksOut_ = 0;
    int32_t  lastGapUs_ = 0;
    int32_t  maxJitterUs_ = 0;
    int32_t  maxLateUs_ = 0;
    uint32_t restarts_ = 0;
};

} // namespace crosspad

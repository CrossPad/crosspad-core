// SPDX-License-Identifier: MIT
#pragma once

#include "crosspad/audio/IAudioNode.hpp"
#include "crosspad/sequencer/SequencerTypes.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace crosspad {

/// The metronome, and the clock everything else in the sequencer reads.
///
/// Counting in the render rather than on a timer is the whole point: the click
/// a player hears and the grid a recording is quantised to are then the same
/// number of samples, so they cannot drift apart. A timer-driven click is a few
/// milliseconds late at random, which is exactly the error a musician hears.
///
/// RT-safe by construction -- no allocation, no locks, parameters through
/// atomics -- because mix() runs on the audio thread.
class MetronomeNode : public IAudioNode {
public:
    const char* name() const override { return "Click"; }
    bool isGenerator() const override { return true; }

    void onPrepare(uint32_t /*maxFrames*/, uint32_t sampleRate) override {
        sampleRate_ = sampleRate ? sampleRate : 48000;
        recompute();
    }
    void onSampleRateChange(uint32_t newSr) override {
        sampleRate_ = newSr ? newSr : 48000;
        recompute();
    }

    // ── Settings ──────────────────────────────────────────────────────────
    void setBpm(float bpm) {
        if (bpm < SequencerLimits::MIN_BPM) bpm = SequencerLimits::MIN_BPM;
        if (bpm > SequencerLimits::MAX_BPM) bpm = SequencerLimits::MAX_BPM;
        bpm_.store(bpm, std::memory_order_relaxed);
        recompute();
    }
    float bpm() const { return bpm_.load(std::memory_order_relaxed); }

    /// Beats in a bar, and with barCount() the length the sequencer loops over.
    void setBeatsPerBar(uint8_t b) { beatsPerBar_.store(b ? b : 4, std::memory_order_relaxed); }
    uint8_t beatsPerBar() const { return beatsPerBar_.load(std::memory_order_relaxed); }

    void setBarCount(uint8_t bars) { barCount_.store(bars ? bars : 1, std::memory_order_relaxed); }
    uint8_t barCount() const { return barCount_.load(std::memory_order_relaxed); }

    /// Audible or silent. The clock runs either way -- a player counting a bar
    /// in their head still wants the recording to land on the grid.
    void setAudible(bool on) { audible_.store(on, std::memory_order_relaxed); }
    bool audible() const { return audible_.load(std::memory_order_relaxed); }

    void setLevel(float v) { level_.store(v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v), std::memory_order_relaxed); }

    // ── Transport ─────────────────────────────────────────────────────────
    //
    // Only mix() writes the position and the count-in. Every transport call
    // posts where the clock should be and mix() takes it at the next block
    // start: a call that wrote the position itself would be overwritten by a
    // block already in flight, which stores its own position when it ends.

    void start() { startAt(0); }
    /// Start already @p pos samples into the loop: for a transport that is
    /// not the loop's own, such as a song timeline the pattern is laid on.
    void startAt(uint32_t pos) {
        const uint32_t loop = loopSamples();
        post(loop ? pos % loop : 0, 0);
        running_.store(true, std::memory_order_release);
        fireHook(true);
    }
    /// Start with @p beats of count-in: the click counts them -- audible even
    /// when the click is off, that is what a count-in is for -- while the
    /// position stays at zero, and the loop's beat one is the sample right
    /// after the last of them. Done in the render rather than by whoever asked,
    /// so the hand-over lands on the sample, not on the next poll. The
    /// transport hook hears this start from takeCountInEnded(), at beat one.
    /// @p pos is where the loop starts after it, as for startAt().
    void startCountIn(uint8_t beats, uint32_t pos = 0) {
        countInEnded_.store(false, std::memory_order_relaxed);
        const uint32_t loop = loopSamples();
        post(loop ? pos % loop : 0, samplesPerBeat_ * beats);
        running_.store(true, std::memory_order_release);
    }
    /// True once, from the task that polls the sequencer, after a count-in
    /// has handed over; tells the transport hook then, so MIDI Start goes out
    /// with beat one and never from the audio thread.
    bool takeCountInEnded() {
        if (!countInEnded_.exchange(false, std::memory_order_acq_rel)) return false;
        fireHook(true);
        return true;
    }
    /// Told on every start (at beat one for a count-in) and on stop(), not on
    /// resume(), from the caller's task: the MIDI clock sends Start/Stop at the
    /// moment the click itself moves.
    using TransportHook = void (*)(bool running, void * ctx);
    void setTransportHook(TransportHook fn, void * ctx) {
        hookCtx_ = ctx;
        hook_.store(fn, std::memory_order_release);
    }
    /// MIDI Continue: run on from where the click stopped, without counting in.
    void resume() {
        post(positionSamples(), 0);
        running_.store(true, std::memory_order_release);
    }
    void stop() {
        running_.store(false, std::memory_order_release);
        fireHook(false);
    }
    bool running() const { return running_.load(std::memory_order_acquire); }

    void resetPosition() { post(0, 0); }

    bool countingIn() const { return running() && countInSamples() > 0; }
    /// Samples of count-in still to play, 0 when not counting in: how far
    /// before beat one a hit made now is.
    uint32_t countInSamplesLeft() const { return running() ? countInSamples() : 0; }
    /// Beats still to count, 0 once the loop is running: 4, 3, 2, 1.
    uint8_t countInBeatsLeft() const {
        if (!running()) return 0;
        const uint32_t spb = samplesPerBeat_ ? samplesPerBeat_ : 1;
        return (uint8_t)((countInSamples() + spb - 1) / spb);
    }

    // ── Where we are, for the UI and the recorder ─────────────────────────

    /// Samples since start, wrapped at the loop length. The recorder turns this
    /// into a grid position; the UI turns it into a moving bar.
    uint32_t positionSamples() const {
        return pending() ? cmdPos_.load(std::memory_order_relaxed)
                         : posSamples_.load(std::memory_order_relaxed);
    }
    /// The grid, in samples. The sequencer quantises against this exact number
    /// so a take cannot land on a grid the click does not play.
    uint32_t samplesPerBeat() const { return samplesPerBeat_; }
    uint32_t loopSamples() const {
        return samplesPerBeat_ * beatsPerBar() * barCount();
    }
    uint8_t currentBeat() const {
        const uint32_t spb = samplesPerBeat_ ? samplesPerBeat_ : 1;
        return (uint8_t)((positionSamples() / spb) % beatsPerBar());
    }
    uint8_t currentBar() const {
        const uint32_t spb = samplesPerBeat_ ? samplesPerBeat_ : 1;
        const uint32_t beat = positionSamples() / spb;
        return (uint8_t)((beat / beatsPerBar()) % barCount());
    }

    void mix(float* out, uint32_t frames) override {
        if (!running_.load(std::memory_order_acquire)) return;
        const uint32_t spb  = samplesPerBeat_ ? samplesPerBeat_ : 1;
        const uint32_t loop = loopSamples() ? loopSamples() : spb;
        uint32_t pos = posSamples_.load(std::memory_order_relaxed);
        uint32_t cin = countInLeft_.load(std::memory_order_relaxed);
        takeCommand(pos, cin);
        const uint32_t cinTotal = countInTotal_.load(std::memory_order_relaxed);
        const bool aud = audible_.load(std::memory_order_relaxed);
        const float lvl = level_.load(std::memory_order_relaxed);
        const bool wasCounting = cin > 0;

        for (uint32_t i = 0; i < frames; ++i) {
            /* Counting in, the pre-roll has its own position and the loop's
             * stays at zero until the last pre-roll sample is out. */
            const bool counting = cin > 0;
            const uint32_t at = counting ? cinTotal - cin : pos;
            /* A beat starts exactly where the sample index crosses it, so the
             * click is placed to the sample and never rounded to a block. */
            if (at % spb == 0) {
                clickLeft_ = clickSamples_;
                /* Bar one is a fifth up, which is what makes a count-in
                 * countable without watching the screen. */
                const bool downbeat = ((at / spb) % beatsPerBar()) == 0;
                step_ = (downbeat ? 1500.0f : 1000.0f) * 6.2831853f / (float)sampleRate_;
                phase_ = 0.0f;
            }
            if (clickLeft_ && (aud || counting)) {
                /* Decaying sine: short enough to read as a tick, shaped so it
                 * does not click at the edges of its own envelope. */
                const float env = (float)clickLeft_ / (float)clickSamples_;
                const float s = std::sin(phase_) * env * env * lvl;
                out[i * 2 + 0] += s;
                out[i * 2 + 1] += s;
                phase_ += step_;
            }
            if (clickLeft_) --clickLeft_;
            if (counting) --cin;
            else if (++pos >= loop) pos = 0;
        }
        countInLeft_.store(cin, std::memory_order_relaxed);
        posSamples_.store(pos, std::memory_order_relaxed);
        if (wasCounting && !cin) countInEnded_.store(true, std::memory_order_release);
    }

private:
    /* A seqlock: writers serialise on cmdMutex_ (never taken by mix()) and
     * leave cmdSeq_ odd while the fields are half written; mix() takes a
     * command only from an even, unchanged sequence number. */
    void post(uint32_t pos, uint32_t countIn) {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        const uint32_t seq = cmdSeq_.load(std::memory_order_relaxed);
        cmdSeq_.store(seq + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        cmdPos_.store(pos, std::memory_order_relaxed);
        cmdCountIn_.store(countIn, std::memory_order_relaxed);
        cmdSeq_.store(seq + 2, std::memory_order_release);
    }
    bool pending() const {
        return cmdSeq_.load(std::memory_order_acquire) != appliedSeq_.load(std::memory_order_acquire);
    }
    uint32_t countInSamples() const {
        return pending() ? cmdCountIn_.load(std::memory_order_relaxed)
                         : countInLeft_.load(std::memory_order_relaxed);
    }
    void takeCommand(uint32_t& pos, uint32_t& cin) {
        const uint32_t seq = cmdSeq_.load(std::memory_order_acquire);
        if ((seq & 1u) || seq == appliedSeq_.load(std::memory_order_relaxed)) return;
        const uint32_t p = cmdPos_.load(std::memory_order_relaxed);
        const uint32_t c = cmdCountIn_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (cmdSeq_.load(std::memory_order_relaxed) != seq) return;   // rewritten meanwhile: next block
        pos = p;
        cin = c;
        countInTotal_.store(c, std::memory_order_relaxed);
        clickLeft_ = 0;
        phase_ = 0.0f;
        appliedSeq_.store(seq, std::memory_order_release);
    }
    void fireHook(bool running) {
        if (auto h = hook_.load(std::memory_order_acquire)) h(running, hookCtx_);
    }

    void recompute() {
        const float b = bpm_.load(std::memory_order_relaxed);
        samplesPerBeat_ = (uint32_t)((float)sampleRate_ * 60.0f / (b > 0.0f ? b : 120.0f));
        if (!samplesPerBeat_) samplesPerBeat_ = 1;
        clickSamples_ = sampleRate_ / 50;      // 20 ms
    }

    uint32_t sampleRate_     = 48000;
    uint32_t samplesPerBeat_ = 24000;
    uint32_t clickSamples_   = 960;
    uint32_t clickLeft_      = 0;
    float    phase_          = 0.0f;
    float    step_           = 0.0f;

    std::atomic<float>    bpm_{120.0f};
    std::atomic<float>    level_{0.35f};
    std::atomic<uint8_t>  beatsPerBar_{4};
    std::atomic<uint8_t>  barCount_{SequencerLimits::MAX_SEQ_LENGTH / 4};
    std::atomic<bool>     audible_{true};
    std::atomic<bool>     running_{false};
    std::atomic<uint32_t> posSamples_{0};
    std::atomic<uint32_t> countInLeft_{0};    // pre-roll samples still to play
    std::atomic<uint32_t> countInTotal_{0};
    std::atomic<bool>     countInEnded_{false};
    std::mutex            cmdMutex_;
    std::atomic<uint32_t> cmdSeq_{0};
    std::atomic<uint32_t> cmdPos_{0};
    std::atomic<uint32_t> cmdCountIn_{0};
    std::atomic<uint32_t> appliedSeq_{0};
    std::atomic<TransportHook> hook_{nullptr};
    void * hookCtx_ = nullptr;
};

} // namespace crosspad

/// The platform's one metronome (CrosspadPlatform.cpp on ESP-IDF), declared at
/// global scope where it is defined.
crosspad::MetronomeNode& getMetronome();

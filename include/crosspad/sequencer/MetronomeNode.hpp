// SPDX-License-Identifier: MIT
#pragma once

#include "crosspad/audio/IAudioNode.hpp"
#include "crosspad/sequencer/SequencerTypes.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>

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
    void start() { resetPosition(); running_.store(true, std::memory_order_release); }
    /// MIDI Continue: run on from where the click stopped.
    void resume() { running_.store(true, std::memory_order_release); }
    void stop()  { running_.store(false, std::memory_order_release); }
    bool running() const { return running_.load(std::memory_order_acquire); }

    void resetPosition() {
        posSamples_.store(0, std::memory_order_relaxed);
        clickLeft_ = 0;
        phase_ = 0.0f;
    }

    // ── Where we are, for the UI and the recorder ─────────────────────────

    /// Samples since start, wrapped at the loop length. The recorder turns this
    /// into a grid position; the UI turns it into a moving bar.
    uint32_t positionSamples() const { return posSamples_.load(std::memory_order_relaxed); }
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
        const bool aud = audible_.load(std::memory_order_relaxed);
        const float lvl = level_.load(std::memory_order_relaxed);

        for (uint32_t i = 0; i < frames; ++i) {
            /* A beat starts exactly where the sample index crosses it, so the
             * click is placed to the sample and never rounded to a block. */
            if (pos % spb == 0) {
                clickLeft_ = clickSamples_;
                /* Bar one is a fifth up, which is what makes a count-in
                 * countable without watching the screen. */
                const bool downbeat = ((pos / spb) % beatsPerBar()) == 0;
                step_ = (downbeat ? 1500.0f : 1000.0f) * 6.2831853f / (float)sampleRate_;
                phase_ = 0.0f;
            }
            if (clickLeft_ && aud) {
                /* Decaying sine: short enough to read as a tick, shaped so it
                 * does not click at the edges of its own envelope. */
                const float env = (float)clickLeft_ / (float)clickSamples_;
                const float s = std::sin(phase_) * env * env * lvl;
                out[i * 2 + 0] += s;
                out[i * 2 + 1] += s;
                phase_ += step_;
            }
            if (clickLeft_) --clickLeft_;
            if (++pos >= loop) pos = 0;
        }
        posSamples_.store(pos, std::memory_order_relaxed);
    }

private:
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
};

} // namespace crosspad

/// The platform's one metronome (CrosspadPlatform.cpp on ESP-IDF), declared at
/// global scope where it is defined.
crosspad::MetronomeNode& getMetronome();

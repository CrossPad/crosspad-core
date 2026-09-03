// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file ScopeTap.hpp
 * @brief Single-writer ring of mono float samples for a waveform display.
 *
 * The audio thread pushes the block it just rendered with write(); the UI
 * thread pulls the newest samples with snapshot(). One writer only — the
 * engine that owns the tap — and any number of readers.
 *
 * **A snapshot can tear, and that is the design.** The reader copies from a
 * region the writer is free to overwrite while the copy runs, so a block
 * boundary inside a snapshot can hold samples from two different moments. The
 * write index is atomic (release/acquire), so the *count* is always honest and
 * the copy never runs off the buffer; the samples themselves are plain floats.
 * A lock here would put the UI thread's scheduling in front of the audio
 * thread's deadline to fix a one-frame smudge on a 60 Hz view that is redrawn
 * milliseconds later. Feed the snapshot to scopeTrigger() (dsp/ScopeTrigger.hpp)
 * and draw it; do not use this tap for anything that must be sample-exact.
 *
 * The ring itself is a heap vector, not an inline array: 8192 floats is 32 kB,
 * and the engine that owns a tap is a static object — inline, that block would
 * sit in the S3's internal RAM for the life of the firmware. On the heap it
 * lands wherever the platform's allocator puts it (PSRAM on the S3). It is
 * taken once, on setSampleRate() or on the first write(), and never resized,
 * so the audio thread allocates at most on the block that starts the stream —
 * call setSampleRate() from onPrepare() and it never allocates at all.
 *
 * Usage:
 * ```cpp
 * // audio thread, once per block
 * tap.write(monoBlock, frames);
 *
 * // UI thread, once per frame
 * float view[1024];
 * const uint32_t got = tap.snapshot(view, 1024);   // oldest first
 * ```
 */

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace crosspad {

class ScopeTap {
public:
    /// ~186 ms at 44.1 kHz — far more than the 10 ms window a scope draws, so
    /// a reader that misses a few UI frames still has a continuous history.
    static constexpr uint32_t kCapacity = 8192;

    /**
     * @brief Append a block. RT-safe: no lock, no allocation, wraps in place.
     *
     * A block longer than kCapacity keeps only its newest kCapacity samples;
     * the sample count still advances by @p n, so the fill level stays honest.
     */
    void write(const float* mono, uint32_t n) {
        if (!mono || n == 0) return;
        if (buf_.empty()) reserve();
        const uint64_t w = written_.load(std::memory_order_relaxed);

        uint32_t skip = 0;
        if (n > kCapacity) {
            skip = n - kCapacity;
            n = kCapacity;
        }

        const uint32_t pos = static_cast<uint32_t>((w + skip) % kCapacity);
        const uint32_t first = (pos + n <= kCapacity) ? n : (kCapacity - pos);
        std::memcpy(buf_.data() + pos, mono + skip, first * sizeof(float));
        if (first < n) {
            std::memcpy(buf_.data(), mono + skip + first, (n - first) * sizeof(float));
        }

        written_.store(w + skip + n, std::memory_order_release);
    }

    /**
     * @brief Copy the newest @p n samples, oldest first.
     *
     * @param n capped at kCapacity, and at however much has been written.
     * @return the number of samples actually copied.
     */
    uint32_t snapshot(float* out, uint32_t n) const {
        if (!out || n == 0) return 0;
        if (n > kCapacity) n = kCapacity;

        const uint64_t w = written_.load(std::memory_order_acquire);
        if (static_cast<uint64_t>(n) > w) n = static_cast<uint32_t>(w);
        if (n == 0 || buf_.empty()) return 0;

        const uint32_t pos = static_cast<uint32_t>((w - n) % kCapacity);
        const uint32_t first = (pos + n <= kCapacity) ? n : (kCapacity - pos);
        std::memcpy(out, buf_.data() + pos, first * sizeof(float));
        if (first < n) {
            std::memcpy(out + first, buf_.data(), (n - first) * sizeof(float));
        }
        return n;
    }

    /// Total samples ever written. Wraps after 13 million years at 44.1 kHz.
    uint64_t written() const { return written_.load(std::memory_order_acquire); }

    /// Rate of the samples in the ring, set by whoever writes them.
    uint32_t sampleRate() const { return sampleRate_.load(std::memory_order_relaxed); }
    void setSampleRate(uint32_t hz) {
        sampleRate_.store(hz, std::memory_order_relaxed);
        reserve();
    }

    /// Take the ring up front, so no write() ever has to. Idempotent.
    void reserve() {
        if (buf_.empty()) buf_.assign(kCapacity, 0.0f);
    }

    /// Drop the history. Not for the audio thread — use it before reusing a tap.
    void clear() { written_.store(0, std::memory_order_release); }

private:
    std::vector<float> buf_;
    std::atomic<uint64_t> written_{0};
    std::atomic<uint32_t> sampleRate_{0};
};

} // namespace crosspad

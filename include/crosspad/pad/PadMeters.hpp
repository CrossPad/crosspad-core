// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

namespace crosspad {

/// Each pad's peak, as an engine renders it: linear, 1.0 = the bus's full
/// scale, after the pad's volume and pan and before the mixer's channel.
/// One writer (the render thread, note()) and one reader (take(), which reads
/// the peak since the last take and starts the next one); a peak noted while
/// a take runs shows one read late, which a meter does not mind.
class PadMeters {
public:
    static constexpr uint8_t kPads = 16;

    /// Metering costs the render a pass over each voice's block, so an engine
    /// does it only while someone looks.
    void setEnabled(bool on) { enabled_.store(on, std::memory_order_relaxed); }
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

    /// RT.
    void note(uint8_t pad, float peak)
    {
        if (pad >= kPads) return;
        if (peak > peak_[pad].load(std::memory_order_relaxed)) {
            peak_[pad].store(peak, std::memory_order_relaxed);
        }
    }

    float take(uint8_t pad)
    {
        return pad < kPads ? peak_[pad].exchange(0.0f, std::memory_order_relaxed) : 0.0f;
    }

private:
    std::atomic<bool> enabled_{false};
    std::atomic<float> peak_[kPads] = {};
};

} // namespace crosspad

// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file SampleCursor.hpp
 * @brief Reading one sample at a fractional position, wherever its bytes are.
 *
 * A voice does not care whether the frame it wants is in the resident head or
 * in a block the streamer delivered a moment ago. This is the one place that
 * knows, and it answers in the only way a varispeed engine can use: four taps
 * around a 32.32 position, cubic Hermite between them.
 *
 * Two paths, for the same reason PitchedInstrument has two: when all four taps
 * are inside the head -- which is every frame of every sample short enough to
 * be complete, and the opening third of a second of every other one -- the
 * read is a pointer walk with no bounds test per tap. Anywhere else each tap
 * is resolved on its own, which costs more and happens at one frame in two
 * thousand.
 *
 * Header-only and non-virtual on purpose: this runs per output frame on the
 * render thread, inside a 1.45 ms budget shared by sixteen voices.
 */

#include "crosspad/sampler/StreamWindow.hpp"

#include <cstdint>

namespace crosspad {

class SampleCursor {
public:
    /// Low 32 bits of a phase to [0, 1).
    static constexpr float kFracNorm = 1.0f / 4294967296.0f;

    void bind(const int16_t* head, uint32_t headFrames, uint8_t channels,
              uint32_t totalFrames, const StreamWindow* window)
    {
        head_ = head;
        headFrames_ = headFrames;
        channels_ = channels < 1 ? 1 : (channels > 2 ? 2 : channels);
        totalFrames_ = totalFrames;
        window_ = window;
    }

    void clear() { head_ = nullptr; window_ = nullptr; headFrames_ = totalFrames_ = 0; }

    uint32_t totalFrames() const { return totalFrames_; }
    uint32_t headFrames() const { return headFrames_; }
    uint8_t  channels() const { return channels_; }

    /// Frames that can be read right now: the head, plus whatever the streamer
    /// has delivered. A voice must not step past this.
    uint32_t readableFrames() const
    {
        if (!window_) return headFrames_;
        const uint32_t v = window_->validUpTo();
        return v > headFrames_ ? v : headFrames_;
    }

    /**
     * @brief Interpolated frame at @p phase (32.32 frames).
     *
     * @return false when the four taps are not all available -- the end of
     *         what is readable, or a frame the ring has already recycled. The
     *         voice then stops rather than reading something that is not its
     *         own sample, and the engine counts it.
     */
    bool read(uint64_t phase, float& left, float& right) const
    {
        const int64_t i = static_cast<int64_t>(phase >> 32);
        const float f = static_cast<float>(static_cast<uint32_t>(phase)) * kFracNorm;

        if (i >= 1 && i + 2 < static_cast<int64_t>(headFrames_)) {
            const int16_t* p = head_ + (static_cast<size_t>(i) - 1) * channels_;
            if (channels_ == 1) {
                left = right = hermite4(p[0], p[1], p[2], p[3], f);
            } else {
                left  = hermite4(p[0], p[2], p[4], p[6], f);
                right = hermite4(p[1], p[3], p[5], p[7], f);
            }
            return true;
        }

        float l[4], r[4];
        for (int k = 0; k < 4; ++k) {
            if (!tap(i - 1 + k, l[k], r[k])) return false;
        }
        left  = hermite4(l[0], l[1], l[2], l[3], f);
        right = hermite4(r[0], r[1], r[2], r[3], f);
        return true;
    }

private:
    /// Catmull-Rom over four taps. y1 is the sample at the integer position.
    static float hermite4(float y0, float y1, float y2, float y3, float f)
    {
        const float c0 = y1;
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * f + c2) * f + c1) * f + c0;
    }

    /// One frame, from wherever it lives. Out of range clamps to the edges,
    /// which is what keeps the interpolator from stepping off either end;
    /// a frame that is simply not here yet returns false.
    bool tap(int64_t j, float& l, float& r) const
    {
        if (totalFrames_ == 0) return false;
        if (j < 0) j = 0;
        if (j >= static_cast<int64_t>(totalFrames_)) j = static_cast<int64_t>(totalFrames_) - 1;

        const int16_t* p = nullptr;
        if (j < static_cast<int64_t>(headFrames_)) {
            if (!head_) return false;
            p = head_ + static_cast<size_t>(j) * channels_;
        } else {
            if (!window_) return false;
            p = window_->frame(static_cast<uint32_t>(j));
            if (!p) return false;
        }
        l = p[0];
        r = channels_ == 2 ? p[1] : p[0];
        return true;
    }

    const int16_t*     head_ = nullptr;
    const StreamWindow* window_ = nullptr;
    uint32_t headFrames_ = 0;
    uint32_t totalFrames_ = 0;
    uint8_t  channels_ = 1;
};

} // namespace crosspad

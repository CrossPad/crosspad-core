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
        /* A new sample behind the same cursor: whatever run was remembered
         * belonged to the old one. */
        forgetRun();
    }

    void clear()
    {
        head_ = nullptr; window_ = nullptr; headFrames_ = totalFrames_ = 0;
        forgetRun();
    }

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
    /**
     * @brief The frame at @p i, with no interpolation at all.
     *
     * For a voice stepping exactly one frame a frame -- the file at the
     * engine's rate and no pitch on the pad, which is most of a drum kit.
     * A 4-tap Catmull-Rom at a zero fraction returns the middle tap
     * unchanged, so this is the same samples the interpolated path produces,
     * for one load a channel instead of four plus twenty flops.
     *
     * That matters because the *render* is what caps streaming polyphony on
     * this board: at sixteen streaming voices the pass measured 1450 us of a
     * 1451 us block, and ~82 us of that was each voice's interpolation over
     * PSRAM.
     */
    bool sample(uint32_t i, float& left, float& right) const
    {
        if (i + 1 < headFrames_) {
            return one(head_ + static_cast<size_t>(i) * channels_, left, right);
        }
        if (window_) {
            if (runPtr_ && i >= runFirst_ && i + 1 < runLast_) {
                return one(runPtr_ + static_cast<size_t>(i - runFirst_) * channels_,
                           left, right);
            }
            uint32_t first = 0, last = 0;
            const int16_t* p = window_->run(i, first, last);
            if (p && i + 1 < last) {
                runPtr_ = p; runFirst_ = i; runLast_ = last;
                return one(p, left, right);
            }
            runPtr_ = nullptr;
        }
        /* The seams and the very end: the interpolated path already knows how
         * to resolve a tap wherever it lives, and at a zero fraction it gives
         * the same answer. One frame in thousands comes through here. */
        return read(static_cast<uint64_t>(i) << 32, left, right);
    }

    bool read(uint64_t phase, float& left, float& right) const
    {
        const int64_t i = static_cast<int64_t>(phase >> 32);
        const float f = static_cast<float>(static_cast<uint32_t>(phase)) * kFracNorm;

        if (i >= 1 && i + 2 < static_cast<int64_t>(headFrames_)) {
            return walk(head_ + (static_cast<size_t>(i) - 1) * channels_, f, left, right);
        }

        /* Past the head, the same walk over the streamer's blocks -- as long
         * as all four taps are inside one of them.
         *
         * Resolving a tap costs a shift, a mask and an *atomic* read of how
         * far delivery has got, and doing that four times a frame was most of
         * this engine's render time on the board (measured: rt_max 3054 us at
         * five voices, against 814 for sixteen resident ones). Asking once a
         * frame instead fixed that.
         *
         * Once a frame is still too often. The run `window_->run()` hands
         * back is good for thousands of frames -- a block is 4096 of them --
         * so it is remembered here and the window is only asked again when
         * the voice walks out of it. Two atomic acquire loads on objects in
         * PSRAM, 64 times a block per voice, were what stopped sixteen
         * *streaming* voices fitting the block: the render saturated core 1
         * and the task watchdog took the board down on IDLE1.
         *
         * Safe to cache: the run's bounds come from `validUpTo_`, which only
         * ever grows, and from the ring's floor, which the window will not
         * move under a frame this voice has not passed yet (it keeps a block
         * of history for exactly this reason). The cursor is per voice and
         * only the render thread touches it. */
        if (window_ && i >= 1) {
            const uint32_t left4 = static_cast<uint32_t>(i) - 1;
            if (runPtr_ && left4 >= runFirst_ && static_cast<uint32_t>(i) + 2 < runLast_) {
                return walk(runPtr_ + static_cast<size_t>(left4 - runFirst_) * channels_,
                            f, left, right);
            }
            uint32_t first = 0, last = 0;
            const int16_t* p = window_->run(left4, first, last);
            if (p && static_cast<uint32_t>(i) + 2 < last) {
                /* `p` points at `left4`; remember where the run starts so the
                 * next frames can index off it without asking again. */
                runPtr_   = p;
                runFirst_ = left4;
                runLast_  = last;
                return walk(p, f, left, right);
            }
            runPtr_ = nullptr;
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
    /// Four neighbouring frames from @p p, interpolated. @p p is the tap at
    /// i-1, so the caller has already checked that four frames fit.
    bool walk(const int16_t* p, float f, float& left, float& right) const
    {
        if (channels_ == 1) {
            left = right = hermite4(p[0], p[1], p[2], p[3], f);
        } else {
            left  = hermite4(p[0], p[2], p[4], p[6], f);
            right = hermite4(p[1], p[3], p[5], p[7], f);
        }
        return true;
    }

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

    bool one(const int16_t* p, float& left, float& right) const
    {
        left  = static_cast<float>(p[0]);
        right = channels_ == 2 ? static_cast<float>(p[1]) : left;
        return true;
    }

    void forgetRun() const { runPtr_ = nullptr; runFirst_ = 0; runLast_ = 0; }

    /* The run last handed back by the window, so it is asked once a block
     * rather than once a frame. Mutable: `read()` is const because it does
     * not change what the cursor *plays*, only what it remembers. */
    mutable const int16_t* runPtr_   = nullptr;
    mutable uint32_t       runFirst_ = 0;
    mutable uint32_t       runLast_  = 0;
    uint32_t headFrames_ = 0;
    uint32_t totalFrames_ = 0;
    uint8_t  channels_ = 1;
};

} // namespace crosspad

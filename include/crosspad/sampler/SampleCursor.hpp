// SPDX-License-Identifier: GPL-3.0-or-later
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
        bind(head, 0, headFrames, channels, totalFrames, window);
    }

    /// @p head holds the file's frames [@p headStart, @p headStart + @p headFrames):
    /// a layer trimmed to start later than the file does has its head there.
    void bind(const int16_t* head, uint32_t headStart, uint32_t headFrames, uint8_t channels,
              uint32_t totalFrames, const StreamWindow* window)
    {
        head_ = head;
        headStart_ = headStart;
        headFrames_ = headFrames;
        headEnd_ = headStart + headFrames;
        channels_ = channels < 1 ? 1 : (channels > 2 ? 2 : channels);
        totalFrames_ = totalFrames;
        window_ = window;
        lapFrames_ = 0;
        lapOffset_ = 0;
        /* A new sample behind the same cursor: whatever run was remembered
         * belonged to the old one. */
        forgetRun();
    }

    /**
     * @brief A voice that loops over more than its head: @p lapFrames is the
     *        streamed part of one lap (StreamWindow::lapFrames()).
     *
     * The window numbers its laps one after another; the voice's position
     * wraps. Past the head, a frame is the window's at that position plus
     * the laps behind it (nextLap()), so the ring is only ever asked for
     * frames ahead of where it has been.
     */
    void setLoop(uint32_t lapFrames) { lapFrames_ = lapFrames; lapOffset_ = 0; forgetRun(); }

    /// The voice wrapped back to its loop's start.
    void nextLap()
    {
        if (!lapFrames_) return;
        lapOffset_ += lapFrames_;
        forgetRun();
    }

    /// Where the voice at file frame @p i is in the window's numbering -- what
    /// the streaming thread fills ahead of. A voice in the head on a later lap
    /// is about to want that lap's first streamed frame.
    uint32_t streamFrame(uint32_t i) const
    {
        if (!lapOffset_) return i;
        return (i < headEnd_ ? headEnd_ : i) + lapOffset_;
    }

    void clear()
    {
        head_ = nullptr; window_ = nullptr; headStart_ = headFrames_ = headEnd_ = totalFrames_ = 0;
        lapFrames_ = lapOffset_ = 0;
        forgetRun();
    }

    uint32_t totalFrames() const { return totalFrames_; }
    uint32_t headFrames() const { return headFrames_; }
    uint8_t  channels() const { return channels_; }

    /// One past the last frame that can be read right now: the head, plus
    /// whatever the streamer has delivered. A voice must not step past this.
    uint32_t readableFrames() const
    {
        if (!window_) return headEnd_;
        const uint32_t v = window_->validUpTo();
        return v > headEnd_ ? v : headEnd_;
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
        if (i >= headStart_ && i + 1 < headEnd_) {
            return one(head_ + static_cast<size_t>(i - headStart_) * channels_, left, right);
        }
        /* On a later lap only past the head: below it the window holds the
         * previous lap's tail under these numbers, not this frame. */
        if (window_ && (!lapOffset_ || i >= headEnd_)) {
            const uint32_t w = i + lapOffset_;
            if (runPtr_ && w >= runFirst_ && w + 1 < runLast_) {
                return one(runPtr_ + static_cast<size_t>(w - runFirst_) * channels_,
                           left, right);
            }
            uint32_t first = 0, last = 0;
            const int16_t* p = window_->run(w, first, last);
            if (p && w + 1 < last) {
                runPtr_ = p; runFirst_ = w; runLast_ = last;
                return one(p, left, right);
            }
            runPtr_ = nullptr;
        }
        /* The seams and the very end: the interpolated path already knows how
         * to resolve a tap wherever it lives, and at a zero fraction it gives
         * the same answer. One frame in thousands comes through here. */
        return read(static_cast<uint64_t>(i) << 32, left, right);
    }

    /**
     * @brief The frames from @p i on that sample() would read straight out of
     *        one run -- the head, or one block the streamer delivered.
     *
     * For a voice stepping a frame a frame to take a block's worth with a
     * pointer walk, instead of asking sample() 64 times: @p p is set to frame
     * @p i (frames are channels() samples apart) and the count, at most
     * @p max, is returned. The same pointer, the same frames, the same run
     * remembered, as sample() frame by frame. 0 where sample() would go its
     * slow way -- a seam, the very end, a frame not delivered -- and the
     * voice then asks sample() for that one frame.
     */
    uint32_t span(uint32_t i, uint32_t max, const int16_t*& p) const
    {
        uint32_t end = 0;   // the first frame at or past i that sample() serves some other way
        if (i >= headStart_ && i + 1 < headEnd_) {
            p = head_ + static_cast<size_t>(i - headStart_) * channels_;
            end = headEnd_ - 1;
        } else if (window_ && (!lapOffset_ || i >= headEnd_)) {
            /* The window's numbering, as in sample(): a later lap is the
             * laps behind it further on. */
            const uint32_t w = i + lapOffset_;
            if (!(runPtr_ && w >= runFirst_ && w + 1 < runLast_)) {
                uint32_t first = 0, last = 0;
                const int16_t* q = window_->run(w, first, last);
                if (!q || w + 1 >= last) {
                    runPtr_ = nullptr;
                    return 0;
                }
                runPtr_ = q; runFirst_ = w; runLast_ = last;
            }
            p = runPtr_ + static_cast<size_t>(w - runFirst_) * channels_;
            end = runLast_ - 1 - lapOffset_;
        } else {
            return 0;
        }
        const uint32_t n = end - i;
        return n < max ? n : max;
    }

    bool read(uint64_t phase, float& left, float& right) const
    {
        const int64_t i = static_cast<int64_t>(phase >> 32);
        const float f = static_cast<float>(static_cast<uint32_t>(phase)) * kFracNorm;

        if (i >= static_cast<int64_t>(headStart_) + 1 && i + 2 < static_cast<int64_t>(headEnd_)) {
            return walk(head_ + (static_cast<size_t>(i) - 1 - headStart_) * channels_, f, left, right);
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
        if (window_ && i >= 1 && (!lapOffset_ || i - 1 >= static_cast<int64_t>(headEnd_))) {
            const uint32_t left4 = static_cast<uint32_t>(i) - 1 + lapOffset_;
            const uint32_t w = static_cast<uint32_t>(i) + lapOffset_;
            if (runPtr_ && left4 >= runFirst_ && w + 2 < runLast_) {
                return walk(runPtr_ + static_cast<size_t>(left4 - runFirst_) * channels_,
                            f, left, right);
            }
            uint32_t first = 0, last = 0;
            const int16_t* p = window_->run(left4, first, last);
            if (p && w + 2 < last) {
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

    /// The buffer around frame @p i whose four taps read() would walk by
    /// pointer: @p base is frame @p first's, and frames up to @p end are in
    /// it -- the voice's numbering, a later lap's run included. The head
    /// where read() takes it, else the window's run. False where read()
    /// takes its slow path. What a run of interpolated frames is walked over
    /// (the engine's pitched kernel); a frame i is in it while i + 2 < @p end.
    bool runAt(uint32_t i, const int16_t*& base, uint32_t& first, uint32_t& end) const
    {
        if (head_ && i >= headStart_ + 1 && i + 2 < headEnd_) {
            base = head_;
            first = headStart_;
            end = headEnd_;
            return true;
        }
        /* On a later lap only past the head, as in read(): below it the
         * window holds the previous lap's tail under these numbers. Without
         * the laps a streamed pitched loop fell to read() frame by frame on
         * its second time round -- ~170 us a voice a block, and sixteen of
         * them held core 1 until the task watchdog fired on IDLE1. */
        if (!window_ || i < 1 || (lapOffset_ && i - 1 < headEnd_)) return false;
        const uint32_t left4 = i - 1 + lapOffset_;
        const uint32_t w = i + lapOffset_;
        if (!(runPtr_ && left4 >= runFirst_ && w + 2 < runLast_)) {
            uint32_t f = 0, last = 0;
            const int16_t* p = window_->run(left4, f, last);
            if (!p || w + 2 >= last) {
                runPtr_ = nullptr;
                return false;
            }
            runPtr_ = p;
            runFirst_ = left4;
            runLast_ = last;
        }
        /* Back in the voice's numbering. `first` may wrap below zero: the
         * caller only ever subtracts it from a frame of this lap, which
         * unwraps it. */
        base = runPtr_;
        first = runFirst_ - lapOffset_;
        end = runLast_ - lapOffset_;
        return true;
    }

    /// hermite4() over four int16 taps, to the bit: for whole numbers of this
    /// size every coefficient hermite4() works out in float is exact (each
    /// step a multiple of 0.5 well inside 24 bits), so it is the same number
    /// worked out in integers and converted once. That keeps the walk's
    /// arithmetic in the integer registers -- the S3 has sixteen float ones,
    /// and the four constants of the float form were reloaded every frame.
    static float hermite4i(int32_t y0, int32_t y1, int32_t y2, int32_t y3, float f)
    {
        const float c0 = static_cast<float>(y1);
        const float c1 = static_cast<float>(y2 - y0) * 0.5f;
        const float c2 = static_cast<float>(2 * y0 - 5 * y1 + 4 * y2 - y3) * 0.5f;
        const float c3 = static_cast<float>(y3 - y0 + 3 * (y1 - y2)) * 0.5f;
        return ((c3 * f + c2) * f + c1) * f + c0;
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

private:
    /// Four neighbouring frames from @p p, interpolated. @p p is the tap at
    /// i-1, so the caller has already checked that four frames fit.
    bool walk(const int16_t* p, float f, float& left, float& right) const
    {
        if (channels_ == 1) {
            left = right = hermite4i(p[0], p[1], p[2], p[3], f);
        } else {
            left  = hermite4i(p[0], p[2], p[4], p[6], f);
            right = hermite4i(p[1], p[3], p[5], p[7], f);
        }
        return true;
    }

    /// One frame, from wherever it lives. Out of range clamps to the edges,
    /// which is what keeps the interpolator from stepping off either end;
    /// a frame that is simply not here yet returns false.
    bool tap(int64_t j, float& l, float& r) const
    {
        if (totalFrames_ == 0) return false;
        /* Before the head is before the trim: the frame a voice starts on is
         * its edge, as frame 0 is the file's. */
        if (j < static_cast<int64_t>(headStart_)) j = headStart_;
        /* A loop's end is not an edge: the window carries the next lap on. */
        if (!lapFrames_ && j >= static_cast<int64_t>(totalFrames_)) j = static_cast<int64_t>(totalFrames_) - 1;

        const int16_t* p = nullptr;
        if (j < static_cast<int64_t>(headEnd_)) {
            if (!head_) return false;
            p = head_ + static_cast<size_t>(j - headStart_) * channels_;
        } else {
            if (!window_) return false;
            p = window_->frame(static_cast<uint32_t>(j) + lapOffset_);
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
    uint32_t headStart_ = 0;
    uint32_t headFrames_ = 0;
    uint32_t headEnd_ = 0;            ///< headStart_ + headFrames_
    uint32_t totalFrames_ = 0;
    uint32_t lapFrames_ = 0;          ///< streamed frames a lap; 0 = not a streaming loop
    uint32_t lapOffset_ = 0;          ///< laps behind the voice, in the window's numbering
    uint8_t  channels_ = 1;
};

} // namespace crosspad

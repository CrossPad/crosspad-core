// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file StreamWindow.hpp
 * @brief The part of a sample that is on its way from the card, for one voice.
 *
 * A voice that outruns its resident head reads through this. It is a fixed
 * ring of pool blocks covering a sliding run of frames, and it is built around
 * one rule that removes every pointer race between the render thread and the
 * streaming thread:
 *
 *   **Which block holds frame f is arithmetic, not state.** Block number
 *   `n = (f - streamStart) / blockFrames`, held in ring slot `n % blocks`.
 *   The slots are acquired when the voice starts and released when it ends;
 *   their addresses never move while it sounds. The streaming thread only ever
 *   writes *into* a slot, never re-points one.
 *
 * So the only thing the two threads share is one monotonic number --
 * `validUpTo()`, the first frame not yet delivered -- published with a release
 * store and read with an acquire load. The render thread reads below it and
 * never waits; the streaming thread fills above it and never asks permission.
 *
 * Overwriting is bounded by the same arithmetic: block `n` shares its slot
 * with `n - blocks`, so it may only be written once the voice has left
 * `n - blocks`. With four blocks of 2048 frames that is 8192 frames of
 * daylight against the 64 frames a render pass consumes, which is why the
 * render thread can read a slot the streaming thread is allowed to touch
 * without a lock between them.
 *
 * Nothing here knows about audio. It is a window on a file, and the tests
 * drive it with a counting source and check every frame.
 */

#include "crosspad/sampler/BlockPool.hpp"
#include "crosspad/sampler/ISampleSource.hpp"

#include <atomic>
#include <cstdint>

namespace crosspad {

class StreamWindow {
public:
    /**
     * @brief Blocks a voice may hold.
     *
     * Size this against how long a refill pass really takes, not against how
     * often the streaming task is woken. The task asks for the bus every
     * millisecond (`vTaskDelay(1)` at a 1000 Hz tick), but a measured pass
     * with eight voices streaming beside a kit load reading at ~2 MB/s took
     * **100..210 ms** (main/sample_player_port.cpp). A ring that holds less
     * than one such pass is a ring that runs dry whenever the card is busy,
     * which is exactly when a player notices.
     *
     * One of them is history, not lookahead: the interpolator's leftmost tap
     * on the first frame of a block is the last frame of the one before, so
     * the block behind the voice is kept. Eight blocks of 4 kB is therefore
     * seven ahead -- 14336 frames mono / 7168 stereo, 325 ms / 162 ms at
     * 44.1 kHz, and half that for a voice an octave up. That clears the
     * measured worst case; four blocks does not. Eight voices at eight
     * blocks is 256 kB, against the 3.1 MB the engine this replaces held in
     * per-voice stream buffers whether a voice existed or not.
     */
    static constexpr uint8_t kMaxBlocks = 8;
    /* Four, not eight.
     *
     * What limits streaming polyphony on this board is the *per-read*
     * overhead, not the card's bandwidth: measured at sixteen voices, a 4 kB
     * read averaged 1784 us and peaked at 12704 -- 2.2 MB/s out of a 4-bit
     * card at 40 MHz that moves many times that. The streamer is one task
     * issuing those reads in series, so sixteen voices at 43 blocks a second
     * each wanted more read-time than a second has.
     *
     * So: a block four times bigger (the port's kBlockBytes) and four of them
     * instead of eight. Same memory a voice, same milliseconds of lookahead,
     * a quarter of the reads. */
    static constexpr uint8_t kDefaultBlocks = 4;

    StreamWindow() = default;
    ~StreamWindow();

    StreamWindow(const StreamWindow&) = delete;
    StreamWindow& operator=(const StreamWindow&) = delete;

    /// open()'s @p loopFirst for a voice that does not loop.
    static constexpr uint32_t kNoLoop = 0xFFFFFFFFu;

    /**
     * @brief Take @p blocks from @p pool to carry frames from @p streamStart on.
     *
     * Called when a voice starts, on the thread that struck the note. False
     * when the pool has nothing to give -- the caller then plays only the head
     * and counts a refusal, which is a short note rather than a glitch.
     *
     * A looping voice (@p loopFirst, the frame it wraps back to) does not end
     * at @p sampleFrames: the window keeps streaming its laps one after
     * another. Frame numbers past @p sampleFrames are the next lap's, the
     * streamed part of the loop again -- from max(@p loopFirst, @p streamStart)
     * -- so they only ever grow, and a lap is lapFrames() of them. The ring
     * cannot go back, so without this a loop longer than head and ring ended
     * one lap and a head later, the second time it left the head.
     */
    bool open(BlockPool& pool, const char* path, uint8_t channels,
              uint32_t streamStart, uint32_t sampleFrames, uint8_t blocks = kDefaultBlocks,
              uint32_t loopFirst = kNoLoop);

    /// Streamed frames of one lap of a looping voice; 0 for one that does not loop.
    uint32_t lapFrames() const { return lapFrames_; }

    /// Give the blocks back. Audio-safe; call it when the voice ends.
    void close(BlockPool& pool);

    bool isOpen() const { return pool_.load(std::memory_order_acquire) != nullptr; }

    /**
     * @brief Read what the voice will want next. Streaming thread only.
     *
     * Fills forward from where it left off, as far as the ring allows against
     * @p playFrame, at most @p maxBlocks blocks in one call so one voice
     * cannot hold the pass against the others.
     *
     * @return frames newly delivered.
     */
    uint32_t service(ISampleSource& src, uint32_t playFrame, uint8_t maxBlocks = 2);

    /// First frame not yet delivered. Render thread reads this.
    uint32_t validUpTo() const { return validUpTo_.load(std::memory_order_acquire); }

    /// Bytes of frame @p f, or nullptr when it is outside what is delivered
    /// and still in the ring. Render thread.
    const int16_t* frame(uint32_t f) const;

    /**
     * @brief The run of frames around @p f that is one contiguous block.
     *
     * The interpolator wants four neighbouring frames, and resolving each one
     * on its own costs three integer divisions -- twelve a frame, which on
     * this chip was most of the render. This answers once: the block holding
     * @p f, as a pointer and the half-open range of frames it covers, so the
     * caller walks it. nullptr when @p f is not readable.
     *
     * @param first  set to the first frame the returned pointer covers
     * @param last   set to one past the last
     */
    const int16_t* run(uint32_t f, uint32_t& first, uint32_t& last) const;

    /// Frames of one block for this voice's channel count.
    uint32_t blockFrames() const { return blockFrames_; }
    uint32_t streamStart() const { return streamStart_; }

    /// Frames the ring can hold at once.
    uint32_t span() const { return blockFrames_ * blocks_; }

    /// Times service() found the ring full and had nothing to do; useful only
    /// as evidence that the head start is big enough.
    uint32_t idlePasses() const { return idlePasses_; }

private:
    /* Published last by open() and cleared first by close(), both on the
     * streaming thread, and read first by everything the render thread calls.
     * That makes the window safe to open *after* the voice has started, which
     * is the point: taking eight blocks from a shared pool is work, and the
     * thread that strikes a note is the one thread that must not do any. The
     * resident head is 0.74 s and this runs every millisecond, so the ring is
     * always there long before the voice reaches it. */
    std::atomic<BlockPool*> pool_{nullptr};
    const char* path_ = nullptr;        ///< owned by the store's slot, outlives the voice
    /* Whether the ring is one span of memory (one page of the pool) rather
     * than loose blocks. Set by open(); decides whether service() may fill
     * several blocks with one read and whether run() may hand back a run that
     * crosses a block boundary. A fragmented pool costs a seam, not a voice. */
    bool     contig_ = false;
    uint32_t block_[kMaxBlocks] = {};
    int16_t* data_[kMaxBlocks] = {};
    uint8_t  blocks_ = 0;
    uint8_t  channels_ = 1;

    uint32_t blockFrames_  = 0;
    /* blockFrames_ is a power of two by construction (a block is 4096 bytes
     * and a frame is 2 or 4), so the block a frame lives in is a shift and a
     * mask rather than a division. On Xtensa a 32-bit divide is ~30 cycles and
     * the read path wanted twelve of them per frame. */
    uint8_t  blockShift_   = 0;
    uint32_t blockMask_    = 0;
    uint32_t streamStart_  = 0;
    uint32_t sampleFrames_ = 0;
    uint32_t nextBlock_    = 0;         ///< block number the streaming thread fills next
    uint32_t idlePasses_   = 0;
    uint32_t lapFirst_     = 0;         ///< file frame a lap's streamed part starts on
    uint32_t lapFrames_    = 0;         ///< 0: no loop

    /// Frames [first, first + n) of the stream into @p dst, a looping voice's
    /// laps laid end to end. Streaming thread.
    uint32_t readStream(ISampleSource& src, uint32_t first, uint32_t n, int16_t* dst) const;

    std::atomic<uint32_t> validUpTo_{0};
};

} // namespace crosspad

// SPDX-License-Identifier: MIT
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
     * Eight blocks of 4 kB is 16384 frames mono / 8192 stereo -- 370 ms /
     * 185 ms at 44.1 kHz, and half that for a voice an octave up. That clears
     * the measured worst case; four blocks does not. Eight voices at eight
     * blocks is 256 kB, against the 3.1 MB the engine this replaces held in
     * per-voice stream buffers whether a voice existed or not.
     */
    static constexpr uint8_t kMaxBlocks = 8;
    static constexpr uint8_t kDefaultBlocks = 8;

    StreamWindow() = default;
    ~StreamWindow();

    StreamWindow(const StreamWindow&) = delete;
    StreamWindow& operator=(const StreamWindow&) = delete;

    /**
     * @brief Take @p blocks from @p pool to carry frames from @p streamStart on.
     *
     * Called when a voice starts, on the thread that struck the note. False
     * when the pool has nothing to give -- the caller then plays only the head
     * and counts a refusal, which is a short note rather than a glitch.
     */
    bool open(BlockPool& pool, const char* path, uint8_t channels,
              uint32_t streamStart, uint32_t sampleFrames, uint8_t blocks = kDefaultBlocks);

    /// Give the blocks back. Audio-safe; call it when the voice ends.
    void close(BlockPool& pool);

    bool isOpen() const { return pool_ != nullptr; }

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

    /// Frames of one block for this voice's channel count.
    uint32_t blockFrames() const { return blockFrames_; }
    uint32_t streamStart() const { return streamStart_; }

    /// Frames the ring can hold at once.
    uint32_t span() const { return blockFrames_ * blocks_; }

    /// Times service() found the ring full and had nothing to do; useful only
    /// as evidence that the head start is big enough.
    uint32_t idlePasses() const { return idlePasses_; }

private:
    BlockPool* pool_ = nullptr;
    const char* path_ = nullptr;        ///< owned by the store's slot, outlives the voice
    uint32_t block_[kMaxBlocks] = {};
    int16_t* data_[kMaxBlocks] = {};
    uint8_t  blocks_ = 0;
    uint8_t  channels_ = 1;

    uint32_t blockFrames_  = 0;
    uint32_t streamStart_  = 0;
    uint32_t sampleFrames_ = 0;
    uint32_t nextBlock_    = 0;         ///< block number the streaming thread fills next
    uint32_t idlePasses_   = 0;

    std::atomic<uint32_t> validUpTo_{0};
};

} // namespace crosspad

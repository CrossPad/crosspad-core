// SPDX-License-Identifier: MIT
#include "crosspad/sampler/StreamWindow.hpp"

namespace crosspad {

StreamWindow::~StreamWindow()
{
    /* A window outliving its pool would be a bug in the owner, not something
     * to paper over here: blocks are given back by close(), explicitly, on a
     * thread that is allowed to. */
}

bool StreamWindow::open(BlockPool& pool, const char* path, uint8_t channels,
                        uint32_t streamStart, uint32_t sampleFrames, uint8_t blocks)
{
    close(pool);
    if (!path || channels == 0 || blocks == 0 || streamStart >= sampleFrames) return false;
    if (blocks > kMaxBlocks) blocks = kMaxBlocks;

    const uint32_t frames = static_cast<uint32_t>(pool.blockBytes() / (channels * sizeof(int16_t)));
    if (frames == 0) return false;

    for (uint8_t i = 0; i < blocks; ++i) {
        const uint32_t b = pool.acquire();
        if (b == BlockPool::kInvalid) {
            /* Partly open is worse than not open: give back what was taken so
             * the next voice can have it, and let the caller play the head. */
            for (uint8_t j = 0; j < i; ++j) pool.release(block_[j]);
            return false;
        }
        block_[i] = b;
        data_[i] = static_cast<int16_t*>(pool.data(b));
    }

    pool_ = &pool;
    path_ = path;
    blocks_ = blocks;
    channels_ = channels;
    blockFrames_ = frames;
    streamStart_ = streamStart;
    sampleFrames_ = sampleFrames;
    nextBlock_ = 0;
    idlePasses_ = 0;
    validUpTo_.store(streamStart, std::memory_order_release);
    return true;
}

void StreamWindow::close(BlockPool& pool)
{
    for (uint8_t i = 0; i < blocks_; ++i) pool.release(block_[i]);
    blocks_ = 0;
    pool_ = nullptr;
    path_ = nullptr;
    nextBlock_ = 0;
    validUpTo_.store(0, std::memory_order_release);
}

uint32_t StreamWindow::service(ISampleSource& src, uint32_t playFrame, uint8_t maxBlocks)
{
    if (!pool_ || !path_) return 0;

    /* The block the voice is reading from. A voice still inside the head has
     * not reached the stream at all, so block 0 is what it wants next. */
    const uint32_t playBlock = playFrame <= streamStart_
                                   ? 0
                                   : (playFrame - streamStart_) / blockFrames_;

    uint32_t delivered = 0;
    uint8_t done = 0;
    while (done < maxBlocks) {
        const uint32_t n = nextBlock_;
        const uint32_t first = streamStart_ + n * blockFrames_;
        if (first >= sampleFrames_) break;              // the file is all delivered
        /* Slot n % blocks_ still carries block n - blocks_. Writing it now
         * would pull the floor out from under a voice that has not left it. */
        if (n >= playBlock + blocks_) { ++idlePasses_; break; }

        const uint32_t want = (sampleFrames_ - first) < blockFrames_
                                  ? (sampleFrames_ - first) : blockFrames_;
        const uint32_t got = src.read(path_, first, want, data_[n % blocks_]);
        if (got == 0) break;                            // the card said nothing; try next pass

        nextBlock_ = n + 1;
        delivered += got;
        /* Published last, and only after the bytes are in place: this is the
         * one word the render thread reads. */
        validUpTo_.store(first + got, std::memory_order_release);
        if (got < want) break;                          // short read: the file ended early
        ++done;
    }
    return delivered;
}

const int16_t* StreamWindow::frame(uint32_t f) const
{
    if (!pool_ || f < streamStart_) return nullptr;
    const uint32_t valid = validUpTo_.load(std::memory_order_acquire);
    if (f >= valid || valid <= streamStart_) return nullptr;

    const uint32_t rel = f - streamStart_;
    const uint32_t n = rel / blockFrames_;
    /* How far back the ring still reaches, derived from the one word the two
     * threads share rather than from nextBlock_, which only the streaming
     * thread may read. Below it is a voice that went backwards -- a loop into
     * streamed territory -- and the caller, not this, decides what to do.
     *
     * Deliberately conservative: a block the streaming thread is writing
     * right now is not yet counted in `valid`, so `hi` is one short of what
     * the ring really holds, and the frame this refuses was going to be
     * overwritten next anyway. */
    const uint32_t hi = (valid - 1 - streamStart_) / blockFrames_;
    const uint32_t oldest = hi >= blocks_ ? hi - (blocks_ - 1) : 0;
    if (n < oldest) return nullptr;
    return data_[n % blocks_] + static_cast<size_t>(rel % blockFrames_) * channels_;
}

} // namespace crosspad

// SPDX-License-Identifier: GPL-3.0-or-later
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
    /* A block that is not a power of two frames would put a division back in
     * the render path, so it is refused rather than quietly paid for. */
    if ((frames & (frames - 1)) != 0) return false;
    uint8_t shift = 0;
    while ((1u << shift) < frames) ++shift;

    /* One contiguous run if the pool can give one.
     *
     * Blocks of a page sit next to each other, and the port sizes a ring to
     * exactly a page, so the whole ring becomes one span. That buys two
     * things the render and the streamer both want: `service()` fills several
     * blocks with a single read, and `run()` can hand the cursor a run that
     * crosses block boundaries instead of stopping at every one.
     *
     * Falling back to loose blocks rather than refusing: a fragmented pool
     * should cost a seam, not a voice. */
    contig_ = false;
    const uint32_t runFirst = pool.acquireRun(blocks);
    if (runFirst != BlockPool::kInvalid) {
        auto* base = static_cast<int16_t*>(pool.data(runFirst));
        const size_t step = static_cast<size_t>(frames) * channels;
        for (uint8_t i = 0; i < blocks; ++i) {
            block_[i] = runFirst + i;
            data_[i]  = base + static_cast<size_t>(i) * step;
        }
        contig_ = true;
    } else {
        for (uint8_t i = 0; i < blocks; ++i) {
            const uint32_t b = pool.acquire();
            if (b == BlockPool::kInvalid) {
                /* Partly open is worse than not open: give back what was taken
                 * so the next voice can have it, and let the caller play the
                 * head. */
                for (uint8_t j = 0; j < i; ++j) pool.release(block_[j]);
                return false;
            }
            block_[i] = b;
            data_[i] = static_cast<int16_t*>(pool.data(b));
        }
    }

    path_ = path;
    blocks_ = blocks;
    channels_ = channels;
    blockFrames_ = frames;
    blockShift_  = shift;
    blockMask_   = frames - 1;
    streamStart_ = streamStart;
    sampleFrames_ = sampleFrames;
    nextBlock_ = 0;
    idlePasses_ = 0;
    validUpTo_.store(streamStart, std::memory_order_release);
    /* Last: everything above is in place before the render thread may look. */
    pool_.store(&pool, std::memory_order_release);
    return true;
}

void StreamWindow::close(BlockPool& pool)
{
    /* First: the render thread stops reading here before a block goes back. */
    pool_.store(nullptr, std::memory_order_release);
    /* Block by block either way: a run's indices are consecutive, and
     * release() accounts for each the same. */
    for (uint8_t i = 0; i < blocks_; ++i) pool.release(block_[i]);
    blocks_ = 0;
    contig_ = false;
    path_ = nullptr;
    nextBlock_ = 0;
    validUpTo_.store(0, std::memory_order_release);
}

uint32_t StreamWindow::service(ISampleSource& src, uint32_t playFrame, uint8_t maxBlocks)
{
    if (!pool_.load(std::memory_order_acquire) || !path_) return 0;

    /* The block the voice is reading from. A voice still inside the head has
     * not reached the stream at all, so block 0 is what it wants next. */
    const uint32_t playBlock = playFrame <= streamStart_
                                   ? 0
                                   : ((playFrame - streamStart_) >> blockShift_);

    uint32_t delivered = 0;
    uint8_t done = 0;
    while (done < maxBlocks) {
        const uint32_t n = nextBlock_;
        const uint32_t first = streamStart_ + (n << blockShift_);
        if (first >= sampleFrames_) break;              // the file is all delivered
        /* Slot n % blocks_ still carries block n - blocks_. Writing it now
         * would pull the floor out from under a voice that has not left it.
         *
         * One block further back than that, though: the interpolator reads
         * four taps around its position, and on the very first frame of block
         * p the leftmost tap is the *last frame of block p - 1*. A rule that
         * only protected block p recycled p - 1 the moment p + blocks_ - 1
         * was written, and the voice died on the seam -- which is exactly what
         * it did, at the first block boundary, every time. So the window
         * keeps one block of history and its lookahead is blocks_ - 1. */
        if (n + 1 >= playBlock + blocks_) { ++idlePasses_; break; }

        /* How many blocks may be filled in one go: the slots have to be
         * consecutive in memory, so up to the ring's wrap, and no further
         * than the lookahead or the end of the file allows. One read of
         * several blocks instead of several reads of one is the whole point
         * of a contiguous ring -- the cost here is per read, not per byte
         * (measured: a 4 kB read averaged 1784 us at sixteen voices). */
        const uint32_t slot = n % blocks_;
        uint32_t span = 1;
        if (contig_) {
            const uint32_t toWrap    = blocks_ - slot;
            const uint32_t toLookahd = (playBlock + blocks_) - (n + 1);
            span = toWrap < toLookahd ? toWrap : toLookahd;
            const uint32_t framesLeft = sampleFrames_ - first;
            const uint32_t blocksLeft = (framesLeft + blockFrames_ - 1) >> blockShift_;
            if (blocksLeft < span) span = blocksLeft;
            const uint32_t left = static_cast<uint32_t>(maxBlocks - done);
            if (left < span) span = left;
            if (span == 0) span = 1;
        }

        const uint32_t wantSpan = span << blockShift_;
        const uint32_t want = (sampleFrames_ - first) < wantSpan
                                  ? (sampleFrames_ - first) : wantSpan;
        const uint32_t got = src.read(path_, first, want, data_[slot]);
        if (got == 0) break;                            // the card said nothing; try next pass

        /* Blocks *covered*, rounded up: a read that ended 188 frames into the
         * second block has nothing more to fetch there, and advancing by the
         * floor made the next turn of the loop read that tail again and count
         * it twice (a 700-frame file reported 888 delivered). `validUpTo_` is
         * the truth about what is readable; a short read means the card gave
         * what it had, which is the same contract one-block-at-a-time had. */
        const uint32_t covered = (got + blockFrames_ - 1) >> blockShift_;
        nextBlock_ = n + (covered ? covered : 1);
        delivered += got;
        /* Published last, and only after the bytes are in place: this is the
         * one word the render thread reads. */
        validUpTo_.store(first + got, std::memory_order_release);
        if (got < want) break;                          // short read: the file ended early
        done = static_cast<uint8_t>(done + (covered ? covered : 1));
    }
    return delivered;
}

const int16_t* StreamWindow::frame(uint32_t f) const
{
    uint32_t first = 0, last = 0;
    return run(f, first, last);
}

const int16_t* StreamWindow::run(uint32_t f, uint32_t& first, uint32_t& last) const
{
    if (!pool_.load(std::memory_order_acquire) || f < streamStart_) return nullptr;
    const uint32_t valid = validUpTo_.load(std::memory_order_acquire);
    if (f >= valid || valid <= streamStart_) return nullptr;

    const uint32_t rel = f - streamStart_;
    const uint32_t n = rel >> blockShift_;
    /* How far back the ring still reaches, derived from the one word the two
     * threads share rather than from nextBlock_, which only the streaming
     * thread may read. Below it is a voice that went backwards -- a loop into
     * streamed territory -- and the caller, not this, decides what to do.
     *
     * Deliberately conservative: a block the streaming thread is writing
     * right now is not yet counted in `valid`, so `hi` is one short of what
     * the ring really holds, and the frame this refuses was going to be
     * overwritten next anyway. */
    const uint32_t hi = (valid - 1 - streamStart_) >> blockShift_;
    const uint32_t oldest = hi >= blocks_ ? hi - (blocks_ - 1) : 0;
    if (n < oldest) return nullptr;

    const uint32_t off = rel & blockMask_;
    const uint32_t slot = n % blocks_;
    first = streamStart_ + (n << blockShift_);
    /* The run ends where the *contiguous* slots do, or where delivery does,
     * whichever comes first -- the caller may read every frame in between
     * with no further checks.
     *
     * With a contiguous ring that is several blocks rather than one, so the
     * interpolator crosses a block boundary on the fast path and the cursor
     * asks again far less often. Without one it is a single block, as before. */
    const uint32_t spanBlocks = contig_ ? (blocks_ - slot) : 1u;
    const uint32_t spanEnd = first + (spanBlocks << blockShift_);
    last = spanEnd < valid ? spanEnd : valid;
    return data_[slot] + static_cast<size_t>(off) * channels_;
}

} // namespace crosspad

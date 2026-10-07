// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file SampleStore.hpp
 * @brief What of each sample is in RAM, and what is still on the card.
 *
 * The engine this replaces answered that question with "the first ~99 kB of
 * every one of twenty-four slots, allocated at init whether a kit used them or
 * not", and a note that wanted the rest went to the card at the moment it was
 * struck -- an open, a directory walk and a read inside the time the hit was
 * supposed to sound. That is where its latency and its underruns came from.
 *
 * Here every sample keeps a **head** in RAM: its first frames, read once when
 * the kit loads. A note-on therefore never touches the card -- it reads the
 * head -- and the streamer has the whole length of that head to deliver what
 * comes after. A head of 64 kB is 0.37 s of stereo 44.1 kHz; measured over
 * the twenty-three kits on this author's card, heads for a whole kit come to
 * 0.74-1.56 MB (median 1.05), against the 5.5 MB the old engine held
 * regardless.
 *
 * A sample shorter than its head is **complete**: it never streams at all, and
 * neither the pool nor the card hears about it again. On those same kits that
 * is 17 % of samples at a 64 kB head and 36 % at 128 kB -- every short drum
 * hit, which is most of what a pad is struck for.
 *
 * The rest is streamed, through blocks a voice takes from the BlockPool when
 * it starts and gives back when it ends, so an idle kit holds no stream
 * memory at all.
 *
 * Threading: load() and unload() are the loader's. Everything the render
 * thread calls is const and reads only what load() published. serviceIo() is
 * the streaming thread's.
 */

#include "crosspad/sampler/BlockPool.hpp"
#include "crosspad/sampler/ISampleSource.hpp"

#include <atomic>
#include <cstdint>

namespace crosspad {

/// How much RAM the heads of one kit may take, and how it is divided.
struct HeadPlan {
    /// Per sample, in PCM bytes. A sample shorter than this is held whole.
    uint32_t headBytes = 64 * 1024;
    /// The ceiling for the kit. headBytes is cut (never below headBytesMin)
    /// until the kit fits.
    uint32_t budgetBytes = 2u * 1024 * 1024;
    uint32_t headBytesMin = 16 * 1024;

    /**
     * @brief The head size this kit actually gets.
     *
     * @param sizes PCM bytes of each sample, as the file holds them.
     * @param count how many.
     * @return bytes per head; every sample gets this, or its whole length if
     *         that is shorter. Never above headBytes, never below
     *         headBytesMin -- a kit too big even then is let through at the
     *         floor, because a short head still beats going to the card at
     *         note-on, and the alternative is refusing to load the kit.
     */
    uint32_t resolve(const uint32_t* sizes, uint16_t count) const;

    /// What @p head bytes a head would cost for these samples, in total.
    static uint64_t cost(const uint32_t* sizes, uint16_t count, uint32_t head);
};


/**
 * @brief The samples of one kit: what of each is in RAM, and where the rest is.
 *
 * A slot is a path, its header, and a head buffer. Slots are built by the
 * loader (load()) and read by everyone; nothing but the loader ever changes
 * one, and it only does so while no voice refers to it.
 *
 * The head buffers are exact-size allocations, not pool blocks: a head is
 * taken once per kit and lives as long as the kit does, which is the heap's
 * natural unit of work, and a contiguous head is what makes the interpolator's
 * fast path a pointer walk. The pool serves the streaming rings instead,
 * which come and go with every voice.
 */
class SampleStore {
public:
    /**
     * @brief Distinct samples one kit may hold.
     *
     * Not sixteen pads times eight layers: a slot is a *path*, and a kit that
     * puts the same file on four pads pays for one. The widest kit measured
     * on this author's card has 24 samples, and the Sampler's own editor
     * assigns one per pad. Sixty-four leaves room for a kit nobody has made
     * yet without charging the board for it -- every slot is a path and a
     * header, and the table is in RAM for as long as the kit is.
     */
    static constexpr uint16_t kMaxSlots = 64;
    static constexpr uint16_t kNoSlot   = 0xFFFF;
    /// Paths are kept whole: the card's own, UTF-8, with the mount point.
    static constexpr size_t   kMaxPath  = 160;

    struct Slot {
        char       path[kMaxPath] = {};
        SampleDesc desc;
        const int16_t* head = nullptr;   ///< headFrames * desc.channels samples
        uint32_t   headFrames = 0;
        bool       complete = false;     ///< the head is the whole sample
        bool       used = false;
    };

    struct Stats {
        uint16_t slots = 0;
        uint16_t completeSlots = 0;
        size_t   headBytes = 0;
        uint32_t headFramesPlanned = 0;
        uint32_t readFailures = 0;
    };

    SampleStore() = default;
    ~SampleStore();

    SampleStore(const SampleStore&) = delete;
    SampleStore& operator=(const SampleStore&) = delete;

    /// Where heads come from and where bytes come from. Loader thread, once.
    void bind(ISampleSource* src, BlockPool::AllocFn alloc, BlockPool::FreeFn free, void* ctx);

    /// Give back every head. Only legal with no voice reading one.
    void unload();

    /**
     * @brief Drop every head **without freeing it**.
     *
     * For when the allocator took the memory back wholesale: the engine arena
     * is one block that gets re-registered, and a pointer from before that is
     * not one the heap knows any more -- freeing it later asserts inside
     * multi_heap_free. So the owner of the arena tells the store to forget,
     * and nothing is handed back one piece at a time.
     *
     * `BlockPool::forgetPages()` exists for the same reason and was written
     * first; this is the half that was missed, and a pitched kit loading
     * between two sample kits found it (the pitched engine resets the arena,
     * and the next take's unload() then freed stale heads).
     */
    void forget();

    /**
     * @brief Take a path into a slot, reading its head.
     *
     * Loader thread. A path already held is returned as the slot it is in
     * rather than read twice -- a kit that puts the same file on four pads
     * pays for one head.
     *
     * @param headFrames what HeadPlan resolved, in frames; the slot keeps
     *        min(headFrames, length).
     * @return the slot, or kNoSlot when the table is full, the file is
     *         unreadable or the head could not be allocated.
     */
    uint16_t load(const char* path, uint32_t headFrames);

    const Slot* slot(uint16_t i) const
    {
        return (i < kMaxSlots && slots_[i].used) ? &slots_[i] : nullptr;
    }

    uint16_t slotCount() const { return count_; }
    Stats stats() const;

    /// The source, for the streaming thread's own reads.
    ISampleSource* source() const { return src_; }

private:
    ISampleSource*     src_   = nullptr;
    BlockPool::AllocFn alloc_ = nullptr;
    BlockPool::FreeFn  free_  = nullptr;
    void*              ctx_   = nullptr;

    Slot     slots_[kMaxSlots];
    uint16_t count_ = 0;
    size_t   headBytes_ = 0;
    uint32_t readFailures_ = 0;
};

} // namespace crosspad

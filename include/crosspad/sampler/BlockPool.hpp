// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file BlockPool.hpp
 * @brief Sample memory that is given back: pages taken on demand, small
 *        blocks handed out of them, pages released when nothing is in them.
 *
 * The engine this replaces took its whole arena -- 5.5 MB of the board's
 * 6.2 MB PSRAM pool -- at init and never gave any of it back, whether one pad
 * sounded or none. Twelve of its thirty-two per-voice stream buffers could
 * never be claimed at all, because the polyphony cap is below the buffer
 * count, and they were allocated anyway. That is the memory this pool exists
 * to not hold.
 *
 * Two sizes, for two different jobs:
 *
 *  - a **page** is what the platform heap understands (64 kB by default, a
 *    whole TLSF class on the device, so taking and giving one back does not
 *    fragment the pool the UI lives in);
 *  - a **block** is what a voice reads through (4 kB by default = 2048 mono
 *    frames = 46 ms at 44.1 kHz). Sixteen blocks to a page.
 *
 * Who may call what:
 *
 *  - acquire()/release() are for the audio and streaming threads. They touch
 *    one intrusive free list under a spinlock held for a few instructions and
 *    never reach the heap, so they are safe on the render thread.
 *  - reserve() and trim() reach the platform allocator and belong on a
 *    loader/worker thread. trim() hands back every page that has no block in
 *    use -- that is the "give it back between operations" half.
 *
 * The block table is sized once, at construction, from the ceiling the engine
 * will ever need. Growing it would mean publishing a new array to a thread
 * that may be indexing the old one; a fixed table of 32-bit indices costs
 * 4 bytes a block (a 4 MB ceiling is 4 kB of table) and removes the race.
 *
 * Nothing here is specific to the device: the allocator is two function
 * pointers, so the tests run it on malloc and the board runs it on PSRAM.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace crosspad {

class BlockPool {
public:
    using AllocFn = void* (*)(size_t bytes, void* ctx);
    using FreeFn  = void  (*)(void* ptr, void* ctx);

    static constexpr uint32_t kInvalid = 0xFFFFFFFFu;

    struct Config {
        size_t   blockBytes = 4096;       ///< what a voice reads through
        size_t   pageBytes  = 64 * 1024;  ///< what the heap is asked for
        uint32_t maxPages   = 96;         ///< ceiling: maxPages * pageBytes
        AllocFn  alloc = nullptr;
        FreeFn   free  = nullptr;
        void*    ctx   = nullptr;
        /**
         * @brief Where the pool's own two tables come from, if not @c alloc.
         *
         * They must outlive the pages, and on the device the pages come from a
         * pool that is wiped wholesale by re-registering it -- the two sample
         * engines take turns owning it. A table allocated out of that pool is
         * a dangling pointer the moment the pool is reset, and the symptom is
         * not a crash: `reserve()` reads a page array that is no longer its
         * own, decides it already has pages, and hands out nothing. Measured
         * on the board as every streaming voice refused its ring
         * (`norings=187`) while the card was never read at all.
         *
         * So the tables get an allocator of their own -- a couple of kilobytes
         * from the ordinary heap, which nobody resets. Null falls back to
         * @c alloc, which is right for a test and wrong for the board.
         */
        AllocFn  tableAlloc = nullptr;
        FreeFn   tableFree  = nullptr;
        void*    tableCtx   = nullptr;

        /**
         * @brief The platform's own short critical section, if it has one.
         *
         * The free list is guarded for a handful of instructions, which a
         * plain busy-wait flag would seem to be enough for. It is not, and the
         * board said so: the thread that strikes a note runs below the render
         * thread on the same core, so it can be preempted *while holding the
         * lock* -- and the streaming thread, above both on the other core,
         * then spins on a flag whose owner cannot be scheduled until the
         * render gives the core back. Measured as a single noteOn() taking
         * **50.7 ms**, with the render itself healthy at 1219 us.
         *
         * A section that suspends preemption while it is held removes the
         * inversion, and on ESP-IDF that is `portENTER_CRITICAL`. The file
         * port next door reached the same conclusion for the same reason.
         *
         * Null keeps the built-in flag, which is right for a host test and
         * wrong for anything with priorities.
         */
        void (*lock)(void* ctx)   = nullptr;
        void (*unlock)(void* ctx) = nullptr;
        void*  lockCtx            = nullptr;
    };

    struct Stats {
        uint32_t pages        = 0;   ///< pages held right now
        uint32_t blocksTotal  = 0;
        uint32_t blocksUsed   = 0;
        uint32_t blocksPeak   = 0;
        uint32_t acquireFails = 0;   ///< a caller asked and the pool was empty
        size_t   bytesHeld    = 0;   ///< pages * pageBytes: what the heap has lent us
    };

    BlockPool() = default;
    ~BlockPool();

    BlockPool(const BlockPool&) = delete;
    BlockPool& operator=(const BlockPool&) = delete;

    /// Lay out the block table. No page is taken here. False when the
    /// configuration is impossible (no allocator, a page that holds no block).
    bool configure(const Config& cfg);

    /// Hold at least @p blocks, taking pages as needed. Worker thread only.
    /// Returns what it could reach, which may be less than asked.
    uint32_t reserve(uint32_t blocks);

    /// Give back every page with no block of it in use. Worker thread only.
    /// @return pages released.
    uint32_t trim();

    /**
     * @brief Every page is gone, however it went. Nothing is freed.
     *
     * For the one case releaseAll() cannot cover: the memory the pages came
     * from was reclaimed wholesale by its owner, so freeing them individually
     * would be freeing into a heap that no longer knows them. The pool goes
     * back to holding nothing and the next reserve() starts again.
     *
     * The caller is asserting that nothing is reading a block. Worker thread.
     */
    void forgetPages();

    /// Give back everything. Only legal when no block is out -- a block handed
    /// to a voice points into a page this would free. Returns false, and keeps
    /// every page, when something is still holding one.
    bool releaseAll();

    /// One block, or kInvalid when the pool has none free. Never allocates,
    /// never blocks for longer than another thread's list push. Audio-safe.
    uint32_t acquire();

    /// Back to the free list. Audio-safe. Ignores kInvalid.
    void release(uint32_t block);

    /// The bytes of block @p i. Valid for as long as the caller holds it.
    void* data(uint32_t block) const;

    size_t blockBytes() const { return blockBytes_; }
    size_t pageBytes() const { return pageBytes_; }
    uint32_t blocksPerPage() const { return perPage_; }

    Stats stats() const;

private:
    struct Page {
        uint8_t* base = nullptr;
        uint32_t firstBlock = 0;
        uint32_t inUse = 0;      ///< blocks of this page handed out
    };

    /// The platform's critical section where there is one, a busy-wait flag
    /// where there is not. See Config::lock.
    class Spin {
    public:
        explicit Spin(const BlockPool& p) : p_(p) {
            if (p_.lock_) { p_.lock_(p_.lockCtx_); return; }
            while (p_.flag_.test_and_set(std::memory_order_acquire)) { }
        }
        ~Spin() {
            if (p_.unlock_) { p_.unlock_(p_.lockCtx_); return; }
            p_.flag_.clear(std::memory_order_release);
        }
    private:
        const BlockPool& p_;
    };

    /// Under lock_. The lowest empty slot, or maxPages_ when there is none.
    uint32_t freeSlotLocked() const;
    /// Under lock_. Pushes every block of @p page onto the free list.
    void pushPageBlocksLocked(uint32_t page);
    /// Under lock_. Pulls them back off; false (and nothing changed) when one
    /// of them is not on the list, i.e. is in use.
    bool pullPageBlocksLocked(uint32_t page);

    AllocFn alloc_ = nullptr;
    FreeFn  free_  = nullptr;
    void*   ctx_   = nullptr;
    AllocFn tableAlloc_ = nullptr;
    FreeFn  tableFree_  = nullptr;
    void*   tableCtx_   = nullptr;

    size_t   blockBytes_ = 0;
    size_t   pageBytes_  = 0;
    uint32_t perPage_    = 0;
    uint32_t maxPages_   = 0;

    /* A page's blocks are numbered from its slot in this array, so a page may
     * be given back without moving any index a voice is holding: its slot is
     * simply emptied. pageSlots_ is how far the array has ever been used (the
     * bound a scan needs); livePages_ is how many of those slots hold memory. */
    Page*     pages_  = nullptr;      ///< maxPages_ entries, from alloc_
    uint32_t  pageSlots_ = 0;
    uint32_t  livePages_ = 0;

    /// next_[i] is the block after i on the free list, or kInvalid.
    uint32_t* next_ = nullptr;        ///< maxPages_ * perPage_ entries
    uint32_t  freeHead_ = kInvalid;

    void (*lock_)(void*)   = nullptr;
    void (*unlock_)(void*) = nullptr;
    void*  lockCtx_        = nullptr;
    mutable std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
    std::atomic<uint32_t> used_{0};
    std::atomic<uint32_t> peak_{0};
    std::atomic<uint32_t> acquireFails_{0};
};

} // namespace crosspad

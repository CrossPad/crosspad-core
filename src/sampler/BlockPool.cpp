// SPDX-License-Identifier: MIT
#include "crosspad/sampler/BlockPool.hpp"

#include <cstring>

namespace crosspad {

BlockPool::~BlockPool()
{
    if (free_) {
        for (uint32_t p = 0; p < pageSlots_; ++p) {
            if (pages_[p].base) free_(pages_[p].base, ctx_);
        }
    }
    if (tableFree_) {
        if (pages_) tableFree_(pages_, tableCtx_);
        if (next_)  tableFree_(next_, tableCtx_);
    }
    pages_ = nullptr;
    next_ = nullptr;
    pageSlots_ = 0;
    livePages_ = 0;
}

bool BlockPool::configure(const Config& cfg)
{
    if (!cfg.alloc || !cfg.free) return false;
    if (cfg.blockBytes == 0 || cfg.pageBytes < cfg.blockBytes) return false;
    if (cfg.maxPages == 0) return false;
    if (pages_ || next_) return false;        // configure once

    alloc_ = cfg.alloc;
    free_  = cfg.free;
    ctx_   = cfg.ctx;
    tableAlloc_ = cfg.tableAlloc ? cfg.tableAlloc : cfg.alloc;
    tableFree_  = cfg.tableFree  ? cfg.tableFree  : cfg.free;
    tableCtx_   = cfg.tableAlloc ? cfg.tableCtx   : cfg.ctx;
    lock_    = cfg.lock;
    unlock_  = cfg.unlock;
    lockCtx_ = cfg.lockCtx;
    blockBytes_ = cfg.blockBytes;
    pageBytes_  = cfg.pageBytes;
    perPage_    = static_cast<uint32_t>(cfg.pageBytes / cfg.blockBytes);
    maxPages_   = cfg.maxPages;

    pages_ = static_cast<Page*>(tableAlloc_(sizeof(Page) * maxPages_, tableCtx_));
    if (!pages_) return false;
    for (uint32_t i = 0; i < maxPages_; ++i) pages_[i] = Page{};

    const size_t slots = static_cast<size_t>(maxPages_) * perPage_;
    next_ = static_cast<uint32_t*>(tableAlloc_(sizeof(uint32_t) * slots, tableCtx_));
    if (!next_) {
        tableFree_(pages_, tableCtx_);
        pages_ = nullptr;
        return false;
    }
    for (size_t i = 0; i < slots; ++i) next_[i] = kInvalid;

    freeHead_ = kInvalid;
    return true;
}

uint32_t BlockPool::freeSlotLocked() const
{
    for (uint32_t p = 0; p < maxPages_; ++p) {
        if (!pages_[p].base) return p;
    }
    return maxPages_;
}

void BlockPool::pushPageBlocksLocked(uint32_t page)
{
    const uint32_t first = pages_[page].firstBlock;
    for (uint32_t i = 0; i < perPage_; ++i) {
        const uint32_t b = first + i;
        next_[b] = freeHead_;
        freeHead_ = b;
    }
}

bool BlockPool::pullPageBlocksLocked(uint32_t page)
{
    if (pages_[page].inUse != 0) return false;
    const uint32_t first = pages_[page].firstBlock;
    const uint32_t last  = first + perPage_;           // exclusive
    /* Unlink in place: the list is singly linked and the page's blocks are
     * scattered through it after any amount of use. */
    uint32_t prev = kInvalid;
    uint32_t cur  = freeHead_;
    uint32_t removed = 0;
    while (cur != kInvalid) {
        const uint32_t nxt = next_[cur];
        if (cur >= first && cur < last) {
            if (prev == kInvalid) freeHead_ = nxt; else next_[prev] = nxt;
            next_[cur] = kInvalid;
            ++removed;
        } else {
            prev = cur;
        }
        cur = nxt;
    }
    return removed == perPage_;
}

uint32_t BlockPool::reserve(uint32_t blocks)
{
    if (!pages_) return 0;
    for (;;) {
        uint32_t have;
        uint32_t slot;
        {
            Spin g(*this);
            have = livePages_ * perPage_;
            slot = freeSlotLocked();
        }
        if (have >= blocks || slot >= maxPages_) return have;

        /* Outside the lock: the allocator may take one of its own. */
        uint8_t* base = static_cast<uint8_t*>(alloc_(pageBytes_, ctx_));
        if (!base) {
            Spin g(*this);
            return livePages_ * perPage_;
        }
        Spin g(*this);
        /* Another thread may have taken that slot while the heap was busy. */
        const uint32_t p = pages_[slot].base ? freeSlotLocked() : slot;
        if (p >= maxPages_) {
            free_(base, ctx_);
            return livePages_ * perPage_;
        }
        pages_[p].base = base;
        pages_[p].firstBlock = p * perPage_;
        pages_[p].inUse = 0;
        if (p + 1 > pageSlots_) pageSlots_ = p + 1;
        ++livePages_;
        pushPageBlocksLocked(p);
    }
}

uint32_t BlockPool::trim()
{
    if (!pages_) return 0;
    uint32_t released = 0;
    /* Any empty page goes, not only the last one: which page a voice happens
     * to be holding is decided by the order the free list was popped in, so
     * "release from the end" would stop at the first page a single block of
     * which is out and keep every empty page behind it. */
    for (uint32_t p = 0; p < maxPages_; ++p) {
        uint8_t* give = nullptr;
        {
            Spin g(*this);
            if (p >= pageSlots_) break;
            if (!pages_[p].base) continue;
            if (!pullPageBlocksLocked(p)) continue;
            give = pages_[p].base;
            pages_[p] = Page{};
            --livePages_;
            while (pageSlots_ > 0 && !pages_[pageSlots_ - 1].base) --pageSlots_;
        }
        free_(give, ctx_);
        ++released;
    }
    return released;
}

void BlockPool::forgetPages()
{
    if (!pages_) return;
    Spin g(*this);
    for (uint32_t p = 0; p < maxPages_; ++p) pages_[p] = Page{};
    const size_t slots = static_cast<size_t>(maxPages_) * perPage_;
    for (size_t i = 0; i < slots; ++i) next_[i] = kInvalid;
    freeHead_ = kInvalid;
    pageSlots_ = 0;
    livePages_ = 0;
    used_.store(0, std::memory_order_relaxed);
}

bool BlockPool::releaseAll()
{
    if (!pages_) return true;
    if (used_.load(std::memory_order_acquire) != 0) return false;
    trim();
    return used_.load(std::memory_order_acquire) == 0 && livePages_ == 0;
}

uint32_t BlockPool::acquire()
{
    uint32_t b;
    {
        Spin g(*this);
        b = freeHead_;
        if (b == kInvalid) {
            acquireFails_.fetch_add(1, std::memory_order_relaxed);
            return kInvalid;
        }
        freeHead_ = next_[b];
        next_[b] = kInvalid;
        pages_[b / perPage_].inUse++;
    }
    const uint32_t u = used_.fetch_add(1, std::memory_order_relaxed) + 1;
    uint32_t p = peak_.load(std::memory_order_relaxed);
    while (u > p && !peak_.compare_exchange_weak(p, u, std::memory_order_relaxed)) { }
    return b;
}

void BlockPool::release(uint32_t block)
{
    if (block == kInvalid || !pages_) return;
    {
        Spin g(*this);
        const uint32_t p = block / perPage_;
        if (p >= pageSlots_ || !pages_[p].base) return;   // a page already given back
        next_[block] = freeHead_;
        freeHead_ = block;
        Page& pg = pages_[p];
        if (pg.inUse) pg.inUse--;
    }
    used_.fetch_sub(1, std::memory_order_relaxed);
}

void* BlockPool::data(uint32_t block) const
{
    if (block == kInvalid || !pages_) return nullptr;
    const uint32_t p = block / perPage_;
    if (p >= pageSlots_ || !pages_[p].base) return nullptr;
    return pages_[p].base + static_cast<size_t>(block % perPage_) * blockBytes_;
}

BlockPool::Stats BlockPool::stats() const
{
    Stats s;
    {
        Spin g(*this);
        s.pages = livePages_;
        s.blocksTotal = livePages_ * perPage_;
    }
    s.blocksUsed   = used_.load(std::memory_order_relaxed);
    s.blocksPeak   = peak_.load(std::memory_order_relaxed);
    s.acquireFails = acquireFails_.load(std::memory_order_relaxed);
    s.bytesHeld    = static_cast<size_t>(s.pages) * pageBytes_;
    return s;
}

} // namespace crosspad

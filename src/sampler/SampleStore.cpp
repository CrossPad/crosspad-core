// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/sampler/SampleStore.hpp"

#include <cstdio>
#include <cstring>

namespace crosspad {

uint64_t HeadPlan::cost(const uint32_t* sizes, uint16_t count, uint32_t head)
{
    uint64_t total = 0;
    for (uint16_t i = 0; i < count; ++i) {
        total += sizes[i] < head ? sizes[i] : head;
    }
    return total;
}

uint32_t HeadPlan::resolve(const uint32_t* sizes, uint16_t count) const
{
    const uint32_t floorBytes = headBytesMin < headBytes ? headBytesMin : headBytes;
    if (!sizes || count == 0) return headBytes;
    if (cost(sizes, count, headBytes) <= budgetBytes) return headBytes;

    /* Halve down to the floor rather than solve for the exact fit: the head
     * is a time budget for the streamer, and halving it halves that budget in
     * steps a person can reason about (0.37 s, 0.18 s, 0.09 s) instead of
     * landing on whatever number a kit's byte count happens to produce. */
    uint32_t head = headBytes;
    while (head > floorBytes) {
        head /= 2;
        if (head < floorBytes) head = floorBytes;
        if (cost(sizes, count, head) <= budgetBytes) return head;
    }
    return floorBytes;
}


// ── SampleStore ─────────────────────────────────────────────────────────────

SampleStore::~SampleStore()
{
    unload();
}

void SampleStore::bind(ISampleSource* src, BlockPool::AllocFn alloc, BlockPool::FreeFn free, void* ctx)
{
    src_ = src;
    alloc_ = alloc;
    free_ = free;
    ctx_ = ctx;
}

void SampleStore::unload()
{
    if (free_) {
        for (auto& s : slots_) {
            if (s.head) free_(const_cast<int16_t*>(s.head), ctx_);
            s = Slot{};
        }
    } else {
        for (auto& s : slots_) s = Slot{};
    }
    count_ = 0;
    headBytes_ = 0;
}

void SampleStore::forget()
{
    for (auto& s : slots_) s = Slot{};
    count_ = 0;
    headBytes_ = 0;
}

uint16_t SampleStore::load(const char* path, uint32_t headFrames)
{
    if (!src_ || !alloc_ || !free_ || !path || !path[0]) return kNoSlot;
    if (std::strlen(path) >= kMaxPath) return kNoSlot;

    /* The same file on four pads is one head. Checked before the header read,
     * which is a trip to the card. */
    for (uint16_t i = 0; i < kMaxSlots; ++i) {
        if (slots_[i].used && std::strcmp(slots_[i].path, path) == 0) return i;
    }

    uint16_t slot = kNoSlot;
    for (uint16_t i = 0; i < kMaxSlots; ++i) {
        if (!slots_[i].used) { slot = i; break; }
    }
    if (slot == kNoSlot) return kNoSlot;

    SampleDesc desc;
    if (!src_->describe(path, desc) || !desc.valid()) {
        ++readFailures_;
        return kNoSlot;
    }

    const uint32_t want = headFrames < desc.frames ? headFrames : desc.frames;
    if (want == 0) { ++readFailures_; return kNoSlot; }

    const size_t bytes = static_cast<size_t>(want) * desc.channels * sizeof(int16_t);
    auto* head = static_cast<int16_t*>(alloc_(bytes, ctx_));
    if (!head) { ++readFailures_; return kNoSlot; }

    const uint32_t got = src_->read(path, 0, want, head);
    if (got == 0) {
        free_(head, ctx_);
        ++readFailures_;
        return kNoSlot;
    }
    /* A short read is the file ending sooner than its header said. The slot
     * keeps what came back and calls itself complete, rather than carrying a
     * length the streamer would then fail to reach. */
    if (got < want) {
        std::memset(head + static_cast<size_t>(got) * desc.channels, 0,
                    static_cast<size_t>(want - got) * desc.channels * sizeof(int16_t));
        desc.frames = got;
    }

    Slot& s = slots_[slot];
    std::snprintf(s.path, sizeof(s.path), "%s", path);
    s.desc = desc;
    s.head = head;
    s.headFrames = got;
    s.complete = (got >= desc.frames);
    s.used = true;
    ++count_;
    headBytes_ += bytes;
    if (s.complete) src_->done(path);
    return slot;
}

SampleStore::Stats SampleStore::stats() const
{
    Stats st;
    st.slots = count_;
    st.headBytes = headBytes_;
    st.readFailures = readFailures_;
    for (const auto& s : slots_) {
        if (s.used && s.complete) ++st.completeSlots;
    }
    return st;
}

} // namespace crosspad

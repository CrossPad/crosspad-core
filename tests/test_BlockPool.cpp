// SPDX-License-Identifier: MIT
#include <cstring>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "crosspad/sampler/BlockPool.hpp"

#include <atomic>
#include <cstdlib>
#include <set>
#include <thread>
#include <vector>

using crosspad::BlockPool;

namespace {

struct Heap {
    std::atomic<int>    live{0};
    std::atomic<size_t> bytes{0};
    static void* alloc(size_t n, void* ctx) {
        auto* h = static_cast<Heap*>(ctx);
        void* p = std::malloc(n);
        if (p) { h->live++; h->bytes += n; }
        return p;
    }
    static void free_(void* p, void* ctx) {
        auto* h = static_cast<Heap*>(ctx);
        if (p) { h->live--; std::free(p); }
    }
};

BlockPool::Config cfg(Heap& h, size_t block = 1024, size_t page = 4096, uint32_t maxPages = 8) {
    BlockPool::Config c;
    c.blockBytes = block;
    c.pageBytes  = page;
    c.maxPages   = maxPages;
    c.alloc = &Heap::alloc;
    c.free  = &Heap::free_;
    c.ctx   = &h;
    return c;
}

} // namespace

TEST_CASE("BlockPool takes no page until asked", "[blockpool]") {
    Heap h;
    BlockPool pool;
    REQUIRE(pool.configure(cfg(h)));
    // The table itself is two allocations; no page of sample memory yet.
    REQUIRE(pool.stats().pages == 0);
    REQUIRE(pool.stats().bytesHeld == 0);
    REQUIRE(pool.acquire() == BlockPool::kInvalid);
    REQUIRE(pool.stats().acquireFails == 1);
}

TEST_CASE("BlockPool refuses an impossible configuration", "[blockpool]") {
    Heap h;
    BlockPool a, b, c;
    auto bad = cfg(h);
    bad.alloc = nullptr;
    REQUIRE_FALSE(a.configure(bad));

    bad = cfg(h);
    bad.pageBytes = 512;            // smaller than a block
    REQUIRE_FALSE(b.configure(bad));

    REQUIRE(c.configure(cfg(h)));
    REQUIRE_FALSE(c.configure(cfg(h)));   // only once
}

TEST_CASE("BlockPool hands out distinct, correctly spaced blocks", "[blockpool]") {
    Heap h;
    BlockPool pool;
    REQUIRE(pool.configure(cfg(h)));
    REQUIRE(pool.reserve(8) == 8);          // two pages of four
    REQUIRE(pool.stats().pages == 2);
    REQUIRE(pool.stats().bytesHeld == 8192);

    std::set<void*> seen;
    std::vector<uint32_t> held;
    for (int i = 0; i < 8; ++i) {
        const uint32_t b = pool.acquire();
        REQUIRE(b != BlockPool::kInvalid);
        void* p = pool.data(b);
        REQUIRE(p != nullptr);
        REQUIRE(seen.insert(p).second);     // no two blocks share bytes
        // Writing a whole block must not touch its neighbour.
        std::memset(p, 0xA5, pool.blockBytes());
        held.push_back(b);
    }
    REQUIRE(pool.stats().blocksUsed == 8);
    REQUIRE(pool.stats().blocksPeak == 8);
    REQUIRE(pool.acquire() == BlockPool::kInvalid);   // exhausted, not grown

    for (uint32_t b : held) pool.release(b);
    REQUIRE(pool.stats().blocksUsed == 0);
    REQUIRE(pool.stats().blocksPeak == 8);
}

TEST_CASE("BlockPool gives pages back when nothing is in them", "[blockpool]") {
    Heap h;
    BlockPool pool;
    REQUIRE(pool.configure(cfg(h)));
    REQUIRE(pool.reserve(8) == 8);
    const int livePages = h.live.load();

    const uint32_t a = pool.acquire();
    const uint32_t b = pool.acquire();
    REQUIRE(a != BlockPool::kInvalid);
    REQUIRE(b != BlockPool::kInvalid);

    // One page still has a block out, so only the empty one goes back.
    const uint32_t gone = pool.trim();
    REQUIRE(gone == 1);
    REQUIRE(pool.stats().pages == 1);
    REQUIRE(h.live.load() == livePages - 1);
    // What is still held stays readable.
    REQUIRE(pool.data(a) != nullptr);
    std::memset(pool.data(a), 0x5A, pool.blockBytes());

    pool.release(a);
    pool.release(b);
    REQUIRE(pool.releaseAll());
    REQUIRE(pool.stats().pages == 0);
    REQUIRE(pool.stats().bytesHeld == 0);
}

TEST_CASE("BlockPool will not drop a page a voice is reading", "[blockpool]") {
    Heap h;
    BlockPool pool;
    REQUIRE(pool.configure(cfg(h)));
    REQUIRE(pool.reserve(4) == 4);
    const uint32_t held = pool.acquire();
    REQUIRE(held != BlockPool::kInvalid);

    REQUIRE(pool.trim() == 0);
    REQUIRE_FALSE(pool.releaseAll());
    REQUIRE(pool.stats().pages == 1);
    REQUIRE(pool.data(held) != nullptr);

    pool.release(held);
    REQUIRE(pool.trim() == 1);
}

TEST_CASE("BlockPool reserve and trim round-trip without leaking", "[blockpool]") {
    Heap h;
    {
        BlockPool pool;
        REQUIRE(pool.configure(cfg(h)));
        for (int round = 0; round < 20; ++round) {
            REQUIRE(pool.reserve(8) == 8);
            std::vector<uint32_t> held;
            for (int i = 0; i < 5; ++i) held.push_back(pool.acquire());
            for (uint32_t b : held) {
                REQUIRE(b != BlockPool::kInvalid);
                pool.release(b);
            }
            REQUIRE(pool.trim() == 2);
            REQUIRE(pool.stats().pages == 0);
        }
    }
    REQUIRE(h.live.load() == 0);          // the pool freed its table too
}

TEST_CASE("BlockPool acquire and release survive two threads", "[blockpool]") {
    Heap h;
    BlockPool pool;
    REQUIRE(pool.configure(cfg(h, 256, 4096, 16)));
    REQUIRE(pool.reserve(64) == 64);

    std::atomic<bool> go{false};
    std::atomic<int>  failures{0};
    auto churn = [&] {
        while (!go.load()) { }
        for (int i = 0; i < 20000; ++i) {
            const uint32_t b = pool.acquire();
            if (b == BlockPool::kInvalid) continue;
            auto* p = static_cast<uint8_t*>(pool.data(b));
            if (!p) { failures++; continue; }
            // Each holder stamps its own block and checks nobody else did.
            const uint8_t mark = static_cast<uint8_t>(b & 0xFF);
            std::memset(p, mark, pool.blockBytes());
            if (p[0] != mark || p[pool.blockBytes() - 1] != mark) failures++;
            pool.release(b);
        }
    };
    std::thread t1(churn), t2(churn), t3(churn);
    go = true;
    t1.join(); t2.join(); t3.join();

    REQUIRE(failures.load() == 0);
    REQUIRE(pool.stats().blocksUsed == 0);
    REQUIRE(pool.releaseAll());
}

// ── HeadPlan ────────────────────────────────────────────────────────────────
// The numbers below are the author's own kits, measured on 2026-10-06:
// 23 kits, 1783 WAVs, 771 MB. A kit is 15-24 samples of 0.1-16 MB.

#include "crosspad/sampler/SampleStore.hpp"

using crosspad::HeadPlan;

TEST_CASE("HeadPlan keeps the full head when the kit fits", "[headplan]") {
    HeadPlan p;                       // 64 kB heads, 2 MB budget
    // Sixteen samples of 400 kB: heads cost 16 * 64 kB = 1 MB.
    std::vector<uint32_t> sizes(16, 400u * 1024);
    REQUIRE(p.resolve(sizes.data(), (uint16_t)sizes.size()) == 64u * 1024);
    REQUIRE(HeadPlan::cost(sizes.data(), (uint16_t)sizes.size(), 64u * 1024) == 16u * 64 * 1024);
}

TEST_CASE("HeadPlan holds a short sample whole and charges only its length", "[headplan]") {
    HeadPlan p;
    // A kit of short hits: 20 kB each, under the head, so each is complete.
    std::vector<uint32_t> sizes(16, 20u * 1024);
    REQUIRE(HeadPlan::cost(sizes.data(), (uint16_t)sizes.size(), 64u * 1024) == 16u * 20 * 1024);
    REQUIRE(p.resolve(sizes.data(), (uint16_t)sizes.size()) == 64u * 1024);
}

TEST_CASE("HeadPlan cuts the head when a kit will not fit", "[headplan]") {
    HeadPlan p;
    p.budgetBytes = 512 * 1024;       // a tight budget
    std::vector<uint32_t> sizes(24, 2u * 1024 * 1024);
    const uint32_t head = p.resolve(sizes.data(), (uint16_t)sizes.size());
    REQUIRE(head < 64u * 1024);
    REQUIRE(head >= p.headBytesMin);
    REQUIRE(HeadPlan::cost(sizes.data(), (uint16_t)sizes.size(), head) <= p.budgetBytes);
}

TEST_CASE("HeadPlan never goes below its floor, even for an impossible kit", "[headplan]") {
    HeadPlan p;
    p.budgetBytes = 1024;             // smaller than one floor head
    std::vector<uint32_t> sizes(24, 4u * 1024 * 1024);
    // Loading at the floor beats refusing the kit: a short head still keeps
    // the note-on out of the card.
    REQUIRE(p.resolve(sizes.data(), (uint16_t)sizes.size()) == p.headBytesMin);
}

TEST_CASE("HeadPlan on the author's real kits stays near a megabyte", "[headplan]") {
    HeadPlan p;
    // SNIK_kit: the biggest on the card, 16 samples, 25.6 MB of WAV.
    std::vector<uint32_t> snik(16, 1600u * 1024);
    REQUIRE(p.resolve(snik.data(), 16) == 64u * 1024);
    REQUIRE(HeadPlan::cost(snik.data(), 16, 64u * 1024) == 1024u * 1024);

    // Fingerdrumming_Boss: 24 samples, 11.9 MB -- the widest kit.
    std::vector<uint32_t> fdb(24, 500u * 1024);
    REQUIRE(p.resolve(fdb.data(), 24) == 64u * 1024);
    REQUIRE(HeadPlan::cost(fdb.data(), 24, 64u * 1024) == 24u * 64 * 1024);  // 1.5 MB

    // Against the 5 533 984 B the prebuilt engine took whatever was loaded.
    REQUIRE(HeadPlan::cost(fdb.data(), 24, 64u * 1024) < 5533984u / 3);
}

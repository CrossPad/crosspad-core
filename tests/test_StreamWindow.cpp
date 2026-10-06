// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include "crosspad/sampler/StreamWindow.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

using crosspad::BlockPool;
using crosspad::ISampleSource;
using crosspad::SampleDesc;
using crosspad::StreamWindow;

namespace {

void* heapAlloc(size_t n, void*) { return std::malloc(n); }
void  heapFree(void* p, void*)   { std::free(p); }

/// Frame n of channel c is (n * 2 + c), so a reader that lands on the wrong
/// frame, the wrong block or the wrong channel says exactly which.
class CountingSource : public ISampleSource {
public:
    CountingSource(uint32_t frames, uint8_t channels) : frames_(frames), channels_(channels) {}

    bool describe(const char*, SampleDesc& out) override {
        out.frames = frames_; out.sampleRate = 44100; out.channels = channels_; out.bits = 16;
        return true;
    }
    uint32_t read(const char*, uint32_t first, uint32_t n, int16_t* dst) override {
        ++reads;
        lastFirst = first;
        lastCount = n;
        if (first >= frames_) return 0;
        if (first + n > frames_) n = frames_ - first;
        if (fail) return 0;
        for (uint32_t i = 0; i < n; ++i) {
            for (uint8_t c = 0; c < channels_; ++c) {
                dst[i * channels_ + c] = static_cast<int16_t>((first + i) * 2 + c);
            }
        }
        return n;
    }
    static int16_t expect(uint32_t f, uint8_t c) { return static_cast<int16_t>(f * 2 + c); }

    std::atomic<uint32_t> reads{0};
    uint32_t lastFirst = 0, lastCount = 0;
    bool fail = false;

private:
    uint32_t frames_;
    uint8_t  channels_;
};

BlockPool::Config poolCfg(size_t block = 1024, size_t page = 8192, uint32_t maxPages = 8) {
    BlockPool::Config c;
    c.blockBytes = block; c.pageBytes = page; c.maxPages = maxPages;
    c.alloc = &heapAlloc; c.free = &heapFree;
    return c;
}

} // namespace

TEST_CASE("StreamWindow delivers the frames after the head, in order", "[streamwindow]") {
    BlockPool pool;
    REQUIRE(pool.configure(poolCfg()));
    REQUIRE(pool.reserve(8) >= 4);

    const uint32_t head = 500, total = 6000;
    CountingSource src(total, 1);
    StreamWindow w;
    REQUIRE(w.open(pool, "x.wav", 1, head, total, 4));
    REQUIRE(w.blockFrames() == 512);              // 1024 bytes / (1 ch * 2 B)
    REQUIRE(w.span() == 2048);
    REQUIRE(w.validUpTo() == head);
    REQUIRE(w.frame(head) == nullptr);            // nothing delivered yet

    // One pass fills two blocks by default.
    REQUIRE(w.service(src, head) == 1024);
    REQUIRE(w.validUpTo() == head + 1024);
    REQUIRE(src.lastFirst == head + 512);

    for (uint32_t f = head; f < head + 1024; ++f) {
        const int16_t* p = w.frame(f);
        REQUIRE(p != nullptr);
        REQUIRE(*p == CountingSource::expect(f, 0));
    }
    REQUIRE(w.frame(head + 1024) == nullptr);     // not yet
    REQUIRE(w.frame(head - 1) == nullptr);        // that is the head's job

    w.close(pool);
    REQUIRE(pool.stats().blocksUsed == 0);
}

TEST_CASE("StreamWindow keeps stereo frames interleaved and addressable", "[streamwindow]") {
    BlockPool pool;
    REQUIRE(pool.configure(poolCfg()));
    REQUIRE(pool.reserve(8) >= 4);

    CountingSource src(4000, 2);
    StreamWindow w;
    REQUIRE(w.open(pool, "s.wav", 2, 100, 4000, 4));
    REQUIRE(w.blockFrames() == 256);              // 1024 bytes / (2 ch * 2 B)
    // Three blocks ahead, not four: the fourth is the history the
    // interpolator's leftmost tap needs at a block boundary.
    REQUIRE(w.service(src, 100, 4) == 768);

    for (uint32_t f = 100; f < 100 + 768; f += 37) {
        const int16_t* p = w.frame(f);
        REQUIRE(p != nullptr);
        REQUIRE(p[0] == CountingSource::expect(f, 0));
        REQUIRE(p[1] == CountingSource::expect(f, 1));
    }
    w.close(pool);
}

TEST_CASE("StreamWindow will not overwrite a block the voice is still in", "[streamwindow]") {
    BlockPool pool;
    REQUIRE(pool.configure(poolCfg()));
    REQUIRE(pool.reserve(8) >= 4);

    const uint32_t head = 0, total = 100000;
    CountingSource src(total, 1);
    StreamWindow w;
    REQUIRE(w.open(pool, "x.wav", 1, head, total, 4));

    // Fill the ring with the voice still at frame 0: three blocks and no
    // more, however many times it is asked. The fourth slot is the block
    // behind the voice, which the interpolator still reads from.
    for (int i = 0; i < 10; ++i) w.service(src, 0, 4);
    REQUIRE(w.validUpTo() == 3 * 512);
    REQUIRE(w.idlePasses() > 0);

    // What is delivered must still read correctly -- nothing was recycled.
    for (uint32_t f = 0; f < 3 * 512; f += 101) {
        REQUIRE(w.frame(f) != nullptr);
        REQUIRE(*w.frame(f) == CountingSource::expect(f, 0));
    }

    // The voice moves into block 2; blocks 3 and 4 may now be written, and
    // block 1 stays as the history behind it.
    w.service(src, 2 * 512 + 10, 4);
    REQUIRE(w.validUpTo() == 5 * 512);
    for (uint32_t f = 1 * 512; f < 5 * 512; f += 97) {
        REQUIRE(w.frame(f) != nullptr);
        REQUIRE(*w.frame(f) == CountingSource::expect(f, 0));
    }
    w.close(pool);
}

TEST_CASE("StreamWindow stops at the end of the file", "[streamwindow]") {
    BlockPool pool;
    REQUIRE(pool.configure(poolCfg()));
    REQUIRE(pool.reserve(8) >= 4);

    const uint32_t head = 0, total = 700;          // one block and a bit
    CountingSource src(total, 1);
    StreamWindow w;
    REQUIRE(w.open(pool, "x.wav", 1, head, total, 4));

    REQUIRE(w.service(src, 0, 4) == total);
    REQUIRE(w.validUpTo() == total);
    const uint32_t readsAfterAll = src.reads.load();
    // Nothing left: further passes must not go back to the card.
    w.service(src, 0, 4);
    w.service(src, 600, 4);
    REQUIRE(src.reads.load() == readsAfterAll);
    REQUIRE(w.frame(total - 1) != nullptr);
    REQUIRE(w.frame(total) == nullptr);
    w.close(pool);
}

TEST_CASE("StreamWindow opens nothing when the pool is empty", "[streamwindow]") {
    BlockPool pool;
    REQUIRE(pool.configure(poolCfg(1024, 2048, 1)));
    REQUIRE(pool.reserve(2) == 2);

    CountingSource src(10000, 1);
    StreamWindow a, b;
    REQUIRE(a.open(pool, "x.wav", 1, 0, 10000, 2));
    // Only two blocks exist and the first window took both.
    REQUIRE_FALSE(b.open(pool, "y.wav", 1, 0, 10000, 2));
    REQUIRE_FALSE(b.isOpen());
    // And the refusal gave back nothing it had taken part-way.
    REQUIRE(pool.stats().blocksUsed == 2);
    a.close(pool);
    REQUIRE(pool.stats().blocksUsed == 0);
    REQUIRE(b.open(pool, "y.wav", 1, 0, 10000, 2));
    b.close(pool);
}

TEST_CASE("StreamWindow survives a card that answers nothing", "[streamwindow]") {
    BlockPool pool;
    REQUIRE(pool.configure(poolCfg()));
    REQUIRE(pool.reserve(8) >= 4);

    CountingSource src(10000, 1);
    StreamWindow w;
    REQUIRE(w.open(pool, "x.wav", 1, 0, 10000, 4));
    REQUIRE(w.service(src, 0, 2) == 1024);

    src.fail = true;
    REQUIRE(w.service(src, 0, 2) == 0);
    REQUIRE(w.validUpTo() == 1024);                // nothing lost, nothing claimed

    src.fail = false;
    REQUIRE(w.service(src, 600, 2) > 0);
    REQUIRE(w.validUpTo() > 1024);
    w.close(pool);
}

TEST_CASE("StreamWindow reads correctly while the streamer fills it", "[streamwindow]") {
    BlockPool pool;
    REQUIRE(pool.configure(poolCfg(1024, 8192, 8)));
    REQUIRE(pool.reserve(8) >= 4);

    const uint32_t total = 200000;
    CountingSource src(total, 1);
    StreamWindow w;
    REQUIRE(w.open(pool, "x.wav", 1, 0, total, 4));

    std::atomic<uint32_t> play{0};
    std::atomic<bool> stop{false};
    std::atomic<int> wrong{0};

    // The streaming thread, as the BG task would run it.
    std::thread io([&] {
        while (!stop.load(std::memory_order_acquire)) {
            w.service(src, play.load(std::memory_order_acquire), 2);
        }
    });

    // The render thread, walking forward one block at a time and checking
    // every frame it is handed.
    uint32_t f = 0;
    while (f < total) {
        const int16_t* p = w.frame(f);
        if (!p) { std::this_thread::yield(); continue; }
        if (*p != CountingSource::expect(f, 0)) ++wrong;
        ++f;
        play.store(f, std::memory_order_release);
    }
    stop.store(true, std::memory_order_release);
    io.join();

    REQUIRE(wrong.load() == 0);
    REQUIRE(f == total);
    w.close(pool);
    REQUIRE(pool.stats().blocksUsed == 0);
}

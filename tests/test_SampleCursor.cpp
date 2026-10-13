// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "crosspad/sampler/SampleCursor.hpp"

#include <cmath>
#include <cstdlib>
#include <vector>

using crosspad::BlockPool;
using crosspad::ISampleSource;
using crosspad::SampleCursor;
using crosspad::SampleDesc;
using crosspad::StreamWindow;

namespace {

constexpr uint64_t kOne = 1ull << 32;

void* heapAlloc(size_t n, void*) { return std::malloc(n); }
void  heapFree(void* p, void*)   { std::free(p); }

/// A ramp: frame n of channel c is n*2 + c, the same rule the window tests use.
class RampSource : public ISampleSource {
public:
    RampSource(uint32_t frames, uint8_t ch) : frames_(frames), ch_(ch) {}
    bool describe(const char*, SampleDesc& o) override {
        o.frames = frames_; o.sampleRate = 44100; o.channels = ch_; o.bits = 16; return true;
    }
    uint32_t read(const char*, uint32_t first, uint32_t n, int16_t* dst) override {
        if (first >= frames_) return 0;
        if (first + n > frames_) n = frames_ - first;
        for (uint32_t i = 0; i < n; ++i)
            for (uint8_t c = 0; c < ch_; ++c)
                dst[i * ch_ + c] = static_cast<int16_t>((first + i) * 2 + c);
        return n;
    }
private:
    uint32_t frames_; uint8_t ch_;
};

} // namespace

TEST_CASE("SampleCursor reads integer positions exactly", "[cursor]") {
    std::vector<int16_t> head(64);
    for (int i = 0; i < 64; ++i) head[i] = static_cast<int16_t>(i * 2);

    SampleCursor c;
    c.bind(head.data(), 64, 1, 64, nullptr);

    float l = 0, r = 0;
    for (uint32_t i = 1; i + 2 < 64; ++i) {
        REQUIRE(c.read(static_cast<uint64_t>(i) * kOne, l, r));
        REQUIRE(l == static_cast<float>(i * 2));
        REQUIRE(r == l);                 // mono mirrors
    }
}

TEST_CASE("SampleCursor interpolates a ramp linearly", "[cursor]") {
    // Catmull-Rom through four collinear points is that straight line, so a
    // ramp is the one signal whose interpolated values are known exactly.
    std::vector<int16_t> head(64);
    for (int i = 0; i < 64; ++i) head[i] = static_cast<int16_t>(i * 2);

    SampleCursor c;
    c.bind(head.data(), 64, 1, 64, nullptr);

    float l = 0, r = 0;
    for (int i = 2; i < 60; ++i) {
        for (int q = 1; q < 4; ++q) {
            const uint64_t phase = static_cast<uint64_t>(i) * kOne + (kOne / 4) * q;
            REQUIRE(c.read(phase, l, r));
            const float expect = i * 2.0f + 0.5f * q;
            REQUIRE_THAT(l, Catch::Matchers::WithinAbs(expect, 1e-3));
        }
    }
}

TEST_CASE("SampleCursor keeps the two channels apart", "[cursor]") {
    std::vector<int16_t> head(64 * 2);
    for (int i = 0; i < 64; ++i) {
        head[i * 2 + 0] = static_cast<int16_t>(i * 2 + 0);
        head[i * 2 + 1] = static_cast<int16_t>(i * 2 + 1);
    }
    SampleCursor c;
    c.bind(head.data(), 64, 2, 64, nullptr);

    float l = 0, r = 0;
    for (uint32_t i = 1; i + 2 < 64; ++i) {
        REQUIRE(c.read(static_cast<uint64_t>(i) * kOne, l, r));
        REQUIRE(l == static_cast<float>(i * 2));
        REQUIRE(r == static_cast<float>(i * 2 + 1));
    }
}

TEST_CASE("SampleCursor clamps at the edges instead of reading off them", "[cursor]") {
    std::vector<int16_t> head(16);
    for (int i = 0; i < 16; ++i) head[i] = static_cast<int16_t>(100 + i);

    SampleCursor c;
    c.bind(head.data(), 16, 1, 16, nullptr);

    float l = 0, r = 0;
    REQUIRE(c.read(0, l, r));                    // tap -1 clamps to frame 0
    REQUIRE(l == 100.0f);
    REQUIRE(c.read(15ull * kOne, l, r));         // taps 16, 17 clamp to frame 15
    REQUIRE(l == 115.0f);
}

TEST_CASE("SampleCursor walks from the head into the stream without a seam", "[cursor]") {
    BlockPool pool;
    BlockPool::Config pc;
    pc.blockBytes = 1024; pc.pageBytes = 8192; pc.maxPages = 4;
    pc.alloc = &heapAlloc; pc.free = &heapFree;
    REQUIRE(pool.configure(pc));
    REQUIRE(pool.reserve(8) >= 4);

    const uint32_t head = 300, total = 8000;
    RampSource src(total, 1);

    std::vector<int16_t> headBuf(head);
    REQUIRE(src.read("x.wav", 0, head, headBuf.data()) == head);

    StreamWindow w;
    REQUIRE(w.open(pool, "x.wav", 1, head, total, 4));

    SampleCursor c;
    c.bind(headBuf.data(), head, 1, total, &w);

    // Only the head is readable until the streamer has run.
    REQUIRE(c.readableFrames() == head);
    float l = 0, r = 0;
    REQUIRE_FALSE(c.read(static_cast<uint64_t>(head) * kOne, l, r));

    // Walk across the boundary, servicing as a BG pass would.
    uint32_t f = 1;
    while (f + 3 < total) {
        if (!c.read(static_cast<uint64_t>(f) * kOne, l, r)) {
            REQUIRE(w.service(src, f, 2) > 0);
            continue;
        }
        REQUIRE(l == static_cast<float>(f * 2));
        ++f;
    }
    REQUIRE(f + 3 >= total);
    w.close(pool);
}

TEST_CASE("SampleCursor reads a head that starts inside the file, then the stream after it", "[cursor][trim]") {
    BlockPool pool;
    BlockPool::Config pc;
    pc.blockBytes = 1024; pc.pageBytes = 8192; pc.maxPages = 4;
    pc.alloc = &heapAlloc; pc.free = &heapFree;
    REQUIRE(pool.configure(pc));
    REQUIRE(pool.reserve(8) >= 4);

    /* A trim starting at frame 5000: the head holds 5000..5299. */
    const uint32_t start = 5000, head = 300, total = 8000;
    RampSource src(total, 1);
    std::vector<int16_t> headBuf(head);
    REQUIRE(src.read("x.wav", start, head, headBuf.data()) == head);

    StreamWindow w;
    REQUIRE(w.open(pool, "x.wav", 1, start + head, total, 4));
    SampleCursor c;
    c.bind(headBuf.data(), start, head, 1, total, &w);
    REQUIRE(c.readableFrames() == start + head);

    float l = 0, r = 0;
    /* The first frame of the trim, at once and interpolated: the tap before
     * it is the trim's edge, not a frame that is not here. */
    REQUIRE(c.sample(start, l, r));
    REQUIRE(l == static_cast<float>(start * 2));
    REQUIRE(c.read(static_cast<uint64_t>(start) * kOne, l, r));
    REQUIRE(l == static_cast<float>(start * 2));

    uint32_t f = start;
    while (f + 3 < total) {
        if (!c.read(static_cast<uint64_t>(f) * kOne, l, r)) {
            REQUIRE(w.service(src, f, 2) > 0);
            continue;
        }
        REQUIRE(l == static_cast<float>(f * 2));
        ++f;
    }
    REQUIRE(f + 3 >= total);
    w.close(pool);
}

TEST_CASE("SampleCursor interpolates across a block boundary", "[cursor]") {
    BlockPool pool;
    BlockPool::Config pc;
    pc.blockBytes = 1024; pc.pageBytes = 8192; pc.maxPages = 4;
    pc.alloc = &heapAlloc; pc.free = &heapFree;
    REQUIRE(pool.configure(pc));
    REQUIRE(pool.reserve(8) >= 4);

    const uint32_t head = 0, total = 8000;
    RampSource src(total, 1);
    StreamWindow w;
    REQUIRE(w.open(pool, "x.wav", 1, head, total, 4));
    REQUIRE(w.service(src, 0, 4) > 0);

    SampleCursor c;
    c.bind(nullptr, 0, 1, total, &w);

    // 512 frames to a block: the four taps around 511.5 straddle two blocks.
    const uint32_t edge = w.blockFrames();
    float l = 0, r = 0;
    for (uint32_t i = edge - 3; i <= edge + 2; ++i) {
        const uint64_t phase = static_cast<uint64_t>(i) * kOne + kOne / 2;
        REQUIRE(c.read(phase, l, r));
        REQUIRE_THAT(l, Catch::Matchers::WithinAbs(i * 2.0f + 1.0f, 1e-3));
    }
    w.close(pool);
}

TEST_CASE("SampleCursor transposes without resampling the file", "[cursor]") {
    // A 1 kHz sine recorded at 22050 Hz, played by an engine at 44100: the
    // increment is the rate ratio, and the sine must come out at 1 kHz still.
    const uint32_t frames = 2048;
    std::vector<int16_t> head(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        head[i] = static_cast<int16_t>(16000.0 * std::sin(2.0 * M_PI * 1000.0 * i / 22050.0));
    }
    SampleCursor c;
    c.bind(head.data(), frames, 1, frames, nullptr);

    const double ratio = 22050.0 / 44100.0;          // step half a frame per output frame
    const uint64_t inc = static_cast<uint64_t>(ratio * static_cast<double>(kOne));

    // Count zero crossings over a known span of output frames.
    uint64_t phase = kOne;                            // start past the clamped edge
    float prev = 0, l = 0, r = 0;
    int crossings = 0;
    uint32_t rendered = 0;
    while ((phase >> 32) + 3 < frames) {
        REQUIRE(c.read(phase, l, r));
        if (rendered > 0 && ((prev < 0 && l >= 0) || (prev > 0 && l <= 0))) ++crossings;
        prev = l;
        phase += inc;
        ++rendered;
    }
    // 1 kHz at the 44100 Hz output: two crossings per period.
    const double seconds = static_cast<double>(rendered) / 44100.0;
    const double hz = crossings / 2.0 / seconds;
    REQUIRE_THAT(hz, Catch::Matchers::WithinRel(1000.0, 0.02));
}

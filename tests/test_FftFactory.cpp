// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "crosspad/dsp/FftFactory.hpp"
#include "crosspad/dsp/PortableFft.hpp"

#include <cmath>
#include <cstdlib>
#include <vector>

using namespace crosspad;

namespace {

void* alignedAlloc(size_t bytes, size_t align) {
    void* p = nullptr;
    if (align < sizeof(void*)) align = sizeof(void*);
    const size_t rounded = (bytes + align - 1) / align * align;
    if (posix_memalign(&p, align, rounded) != 0) return nullptr;
    return p;
}
void plainFree(void* p) { std::free(p); }

} // namespace

TEST_CASE("createFft: returns a working FFT and currentFft() agrees", "[dsp][fft]") {
    // currentFft() before any createFft() cannot be asserted here — Catch2
    // runs the whole file in one process and another case may have gone
    // first — so this checks the invariant that survives ordering: after a
    // create, currentFft() is exactly what was handed out.
    FftMemory mem;
    mem.alloc = alignedAlloc;
    mem.free = plainFree;

    IFft& fft = createFft(mem);
    REQUIRE(currentFft() == &fft);
    REQUIRE(&createFft(mem) == &fft);   // singleton, not a new object each call

    // Without a registered backend the factory lands on PortableFft, which
    // reaches kMaxFftSize. With one, the size is whatever the backend serves.
    REQUIRE(fft.maxSize() >= kMinFftSize);
    REQUIRE((fft.maxSize() & (fft.maxSize() - 1)) == 0);

    // A real transform: a pure cosine at bin 8 of a 256-point frame.
    constexpr uint32_t n = 256;
    std::vector<float> buf(2 * n, 0.0f);
    for (uint32_t i = 0; i < n; ++i) {
        buf[2 * i] = std::cos(2.0f * static_cast<float>(M_PI) * 8.0f * i / n);
    }
    REQUIRE(fft.forward(buf.data(), n));
    const float mag8 = std::hypot(buf[2 * 8], buf[2 * 8 + 1]);
    const float mag9 = std::hypot(buf[2 * 9], buf[2 * 9 + 1]);
    REQUIRE(mag8 > 100.0f);
    REQUIRE(mag9 < 1.0f);

    // Round trip back to the cosine.
    REQUIRE(fft.inverse(buf.data(), n));
    for (uint32_t i = 0; i < n; ++i) {
        const float want = std::cos(2.0f * static_cast<float>(M_PI) * 8.0f * i / n);
        REQUIRE(std::fabs(buf[2 * i] - want) < 1e-3f);
    }
}

TEST_CASE("createFft: survives an allocator that gives nothing", "[dsp][fft]") {
    // A backend whose table cannot be allocated must fall back rather than
    // hand out an FFT that fails every call; with no backend registered,
    // PortableFft never touches the callbacks at all.
    FftMemory none;
    IFft& fft = createFft(none);
    std::vector<float> buf(2 * 64, 0.0f);
    buf[0] = 1.0f;
    REQUIRE(fft.forward(buf.data(), 64));
}

TEST_CASE("createFft: a backend that cannot serve yet is asked again", "[dsp][fft]") {
    static PortableFft accelerated;
    static int asked;
    static bool ready;
    asked = 0;
    ready = false;
    setFftBackend([](const FftMemory&) -> IFft* {
        ++asked;
        return ready ? &accelerated : nullptr;
    });

    FftMemory mem;
    IFft& fallback = createFft(mem);
    REQUIRE(&fallback != &accelerated);

    ready = true;
    REQUIRE(&createFft(mem) == &accelerated);
    REQUIRE(currentFft() == &accelerated);
    REQUIRE(asked == 2);

    setFftBackend(nullptr);
    REQUIRE(&createFft(mem) == &fallback);
}

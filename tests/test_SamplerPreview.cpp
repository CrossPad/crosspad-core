// SPDX-License-Identifier: GPL-3.0-or-later
//
// The audition voice: one file outside the kit, streamed whole from the card
// through the engine's own pool, rendered on its own output (mixPreview), and
// handed over between three threads -- the note thread asks, the streaming
// thread opens and feeds it, the render plays and ends it.
#include <catch2/catch_test_macros.hpp>

#include "crosspad/sampler/SamplerEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

using crosspad::BlockPool;
using crosspad::ISampleSource;
using crosspad::SampleDesc;
using crosspad::SamplerEngine;
using crosspad::SampleStore;

namespace {

void* heapAlloc(size_t n, void*) { return std::malloc(n); }
void  heapFree(void* p, void*)   { std::free(p); }

/// In-memory "files" whose every frame says which file and frame it is, so a
/// test can tell from the output which audition it is hearing. A stall makes
/// every read come back empty, as a card that is busy elsewhere would.
class MemSource : public ISampleSource {
public:
    void add(const std::string& path, uint32_t frames, uint8_t ch, int16_t level, uint32_t rate = 44100) {
        File f;
        f.ch = ch;
        f.rate = rate;
        f.pcm.assign(static_cast<size_t>(frames) * ch, level);
        files_[path] = std::move(f);
    }

    bool describe(const char* path, SampleDesc& o) override {
        ++describes;
        auto it = files_.find(path);
        if (it == files_.end()) return false;
        o.frames = static_cast<uint32_t>(it->second.pcm.size() / it->second.ch);
        o.sampleRate = it->second.rate;
        o.channels = it->second.ch;
        o.bits = 16;
        return true;
    }

    uint32_t read(const char* path, uint32_t first, uint32_t n, int16_t* dst) override {
        if (stall) return 0;
        auto it = files_.find(path);
        if (it == files_.end()) return 0;
        const File& f = it->second;
        const uint32_t total = static_cast<uint32_t>(f.pcm.size() / f.ch);
        if (first >= total) return 0;
        if (first + n > total) n = total - first;
        std::memcpy(dst, f.pcm.data() + static_cast<size_t>(first) * f.ch,
                    static_cast<size_t>(n) * f.ch * sizeof(int16_t));
        framesRead += n;
        return n;
    }

    bool stall = false;
    uint32_t describes = 0;
    uint32_t framesRead = 0;

private:
    struct File { std::vector<int16_t> pcm; uint8_t ch = 1; uint32_t rate = 44100; };
    std::map<std::string, File> files_;
};

struct Rig {
    MemSource     kitSrc;      // the kit's: an audition must never read through it
    MemSource     src;         // the audition's own
    BlockPool     pool;
    SampleStore   store;
    SamplerEngine engine;

    explicit Rig(uint32_t poolBlocks = 64) {
        BlockPool::Config pc;
        pc.blockBytes = 2048;
        pc.pageBytes  = 2048 * 8;
        pc.maxPages   = 16;
        pc.alloc = &heapAlloc;
        pc.free  = &heapFree;
        REQUIRE(pool.configure(pc));
        if (poolBlocks) REQUIRE(pool.reserve(poolBlocks) > 0);
        store.bind(&kitSrc, &heapAlloc, &heapFree, nullptr);
        engine.bind(&pool, &store);
        engine.setPreviewSource(&src);
        engine.onPrepare(64, 44100);
    }

    /// One render block of both outputs, then the streaming pass, as the
    /// board runs them. Returns the audition's peak; the kit's output is
    /// added to @p kitPeak.
    float block(float* kitPeak = nullptr, uint32_t frames = 64) {
        std::vector<float> kit(frames * 2, 0.0f), prev(frames * 2, 0.0f);
        engine.mix(kit.data(), frames);
        engine.mixPreview(prev.data(), frames);
        engine.serviceIo();
        float pk = 0.0f;
        for (float s : prev) pk = std::max(pk, std::fabs(s));
        if (kitPeak) for (float s : kit) *kitPeak = std::max(*kitPeak, std::fabs(s));
        lastL = prev[0];
        return pk;
    }

    float run(uint32_t blocks, float* kitPeak = nullptr) {
        float pk = 0.0f;
        for (uint32_t b = 0; b < blocks; ++b) pk = std::max(pk, block(kitPeak));
        return pk;
    }

    float lastL = 0.0f;
};

constexpr float kLevel(int16_t v) { return static_cast<float>(v) / 32768.0f; }

} // namespace

TEST_CASE("Audition plays a file outside the kit on its own output", "[samplerengine][preview]") {
    Rig rig;
    rig.src.add("/sdcard/a.wav", 20000, 2, 8000);

    REQUIRE(rig.engine.previewStart("/sdcard/a.wav"));
    REQUIRE(rig.engine.previewBusy());

    float kitPeak = 0.0f;
    const float pk = rig.run(20, &kitPeak);
    /* 8000/32768 at unity gain, centred: unity a side, as a pad at centre. */
    REQUIRE(pk > kLevel(8000) - 1e-4f);
    REQUIRE(pk <= kLevel(8000) + 1e-4f);
    REQUIRE(kitPeak == 0.0f);                  // nothing on the kit's channel
    REQUIRE(rig.kitSrc.describes == 0);        // and nothing through the kit's source
    REQUIRE(rig.engine.counters().notesStarted == 0);
}

TEST_CASE("Audition ends by itself and gives its ring back", "[samplerengine][preview]") {
    Rig rig;
    rig.src.add("/sdcard/short.wav", 3000, 1, 4000);

    REQUIRE(rig.engine.previewStart("/sdcard/short.wav"));
    rig.run(4);
    REQUIRE(rig.pool.stats().blocksUsed > 0);  // its ring, from the pool
    rig.run(80);                               // 5120 frames: past the end
    REQUIRE_FALSE(rig.engine.previewBusy());
    REQUIRE(rig.pool.stats().blocksUsed == 0);
    REQUIRE(rig.src.framesRead >= 3000);       // the whole file came off the card
    REQUIRE(rig.run(4) == 0.0f);
}

TEST_CASE("Audition plays only the region it was given", "[samplerengine][preview]") {
    Rig rig;
    rig.src.add("/sdcard/long.wav", 20000, 1, 4000);

    REQUIRE(rig.engine.previewStart("/sdcard/long.wav", 1000, 2000));
    uint32_t sounding = 0;
    for (int b = 0; b < 60; ++b) {
        if (rig.block() > 0.0f) ++sounding;
    }
    /* 1000 frames is 15.6 blocks of 64; a priming block or two before it. */
    REQUIRE(sounding >= 15);
    REQUIRE(sounding <= 17);
    REQUIRE_FALSE(rig.engine.previewBusy());
    REQUIRE(rig.src.framesRead < 4000);        // never read the rest of the file
}

TEST_CASE("Audition stops with a fade and gives its ring back", "[samplerengine][preview]") {
    Rig rig;
    rig.src.add("/sdcard/a.wav", 100000, 2, 8000);

    REQUIRE(rig.engine.previewStart("/sdcard/a.wav"));
    REQUIRE(rig.run(10) > 0.0f);
    rig.engine.previewStop();
    rig.run(6);                                // the fade is 2 ms, 88 frames
    REQUIRE_FALSE(rig.engine.previewBusy());
    REQUIRE(rig.pool.stats().blocksUsed == 0);
    REQUIRE(rig.run(4) == 0.0f);
}

TEST_CASE("Audition: the next one replaces the one sounding", "[samplerengine][preview]") {
    Rig rig;
    rig.src.add("/sdcard/a.wav", 100000, 1, 8000);
    rig.src.add("/sdcard/b.wav", 100000, 1, 2000);

    REQUIRE(rig.engine.previewStart("/sdcard/a.wav"));
    rig.run(10);
    REQUIRE(rig.lastL > kLevel(8000) * 0.5f);

    REQUIRE(rig.engine.previewStart("/sdcard/b.wav"));
    rig.run(10);
    REQUIRE(rig.engine.previewBusy());
    REQUIRE(std::fabs(rig.lastL - kLevel(2000)) < 1e-3f);   // b, centred
    /* One ring at a time: a's was given back before b's was taken. */
    REQUIRE(rig.pool.stats().blocksUsed <= crosspad::StreamWindow::kDefaultBlocks);
}

TEST_CASE("Audition asked for and stopped before it started never sounds", "[samplerengine][preview]") {
    Rig rig;
    rig.src.add("/sdcard/a.wav", 20000, 1, 8000);

    REQUIRE(rig.engine.previewStart("/sdcard/a.wav"));
    rig.engine.previewStop();
    REQUIRE_FALSE(rig.engine.previewBusy());
    REQUIRE(rig.run(10) == 0.0f);
    REQUIRE(rig.src.describes == 0);           // the card was never asked
}

TEST_CASE("Audition of a file that cannot be read is dropped", "[samplerengine][preview]") {
    Rig rig;
    REQUIRE(rig.engine.previewStart("/sdcard/missing.wav"));
    REQUIRE(rig.run(4) == 0.0f);
    REQUIRE_FALSE(rig.engine.previewBusy());
    REQUIRE(rig.pool.stats().blocksUsed == 0);
}

TEST_CASE("Audition with no blocks in the pool is refused, not stuck", "[samplerengine][preview]") {
    Rig rig(0);                                // a pool with no pages
    rig.src.add("/sdcard/a.wav", 20000, 1, 8000);
    REQUIRE(rig.engine.previewStart("/sdcard/a.wav"));
    REQUIRE(rig.run(4) == 0.0f);
    REQUIRE_FALSE(rig.engine.previewBusy());
    REQUIRE(rig.engine.counters().streamRefused == 1);
}

TEST_CASE("Audition waits out a card that falls behind instead of ending", "[samplerengine][preview]") {
    Rig rig;
    rig.src.add("/sdcard/a.wav", 100000, 1, 8000);

    REQUIRE(rig.engine.previewStart("/sdcard/a.wav"));
    REQUIRE(rig.run(4) > 0.0f);
    rig.src.stall = true;
    rig.run(60);                               // far past what the ring held
    REQUIRE(rig.engine.previewBusy());         // a gap, not the end
    REQUIRE(rig.block() == 0.0f);
    rig.src.stall = false;
    rig.run(4);
    REQUIRE(rig.run(4) > 0.0f);                // and it carries on
}

TEST_CASE("Audition plays a 22 kHz file at its own speed", "[samplerengine][preview]") {
    Rig rig;
    rig.src.add("/sdcard/slow.wav", 2205, 1, 4000, 22050);   // 0.1 s

    REQUIRE(rig.engine.previewStart("/sdcard/slow.wav"));
    uint32_t sounding = 0;
    for (int b = 0; b < 200; ++b) {
        if (rig.block() > 0.0f) ++sounding;
    }
    /* 0.1 s at 44.1 kHz is 4410 frames, 69 blocks -- not the 34 a 1:1 read
     * would take. */
    REQUIRE(sounding >= 67);
    REQUIRE(sounding <= 71);
}

TEST_CASE("Audition is refused while the loader has the engine", "[samplerengine][preview]") {
    Rig rig;
    rig.src.add("/sdcard/a.wav", 20000, 1, 8000);
    rig.engine.beginMutation();
    REQUIRE_FALSE(rig.engine.previewStart("/sdcard/a.wav"));
    rig.engine.endMutation();
    REQUIRE(rig.engine.previewStart("/sdcard/a.wav"));
}

TEST_CASE("Audition needs a source and a path that fits", "[samplerengine][preview]") {
    SamplerEngine engine;
    REQUIRE_FALSE(engine.previewStart("/sdcard/a.wav"));    // nothing bound
    Rig rig;
    REQUIRE_FALSE(rig.engine.previewStart(""));
    REQUIRE_FALSE(rig.engine.previewStart(nullptr));
    const std::string tooLong(SampleStore::kMaxPath, 'x');
    REQUIRE_FALSE(rig.engine.previewStart(tooLong.c_str()));
}

TEST_CASE("Audition the render never ends can be abandoned", "[samplerengine][preview]") {
    Rig rig;
    rig.src.add("/sdcard/a.wav", 100000, 1, 8000);

    REQUIRE(rig.engine.previewStart("/sdcard/a.wav"));
    REQUIRE(rig.run(4) > 0.0f);
    rig.engine.previewStop();
    /* The render is parked (the USB audio profile does that): the fade that
     * would end it never runs. */
    for (int i = 0; i < 10; ++i) rig.engine.serviceIo();
    REQUIRE(rig.engine.previewBusy());
    REQUIRE(rig.engine.previewAbandon());
    REQUIRE_FALSE(rig.engine.previewBusy());
    REQUIRE(rig.pool.stats().blocksUsed == 0);
    REQUIRE(rig.run(4) == 0.0f);

    /* And the next one plays as usual. */
    REQUIRE(rig.engine.previewStart("/sdcard/a.wav"));
    REQUIRE(rig.run(6) > 0.0f);
}

// ── Failure paths: every one of them must leave the audition idle, its ring
//    back in the pool, and the next audition (and the pads) working. ──────

namespace {
/// A source whose header is whatever the test says, for files no real
/// reader would hand over: three channels, zero frames, no rate.
class OddSource : public ISampleSource {
public:
    SampleDesc desc;
    bool describe(const char*, SampleDesc& o) override { o = desc; return true; }
    uint32_t read(const char*, uint32_t, uint32_t, int16_t*) override { return 0; }
};
} // namespace

TEST_CASE("Audition of a header no engine can play is dropped", "[samplerengine][preview][fail]") {
    Rig rig;
    OddSource odd;
    rig.engine.setPreviewSource(&odd);
    const SampleDesc bad[] = {
        {1000, 44100, 3, 16},   // three channels
        {0,    44100, 2, 16},   // no frames
        {1000, 0,     2, 16},   // no rate
    };
    for (const SampleDesc& d : bad) {
        odd.desc = d;
        REQUIRE(rig.engine.previewStart("/sdcard/odd.wav"));
        rig.run(3);
        REQUIRE_FALSE(rig.engine.previewBusy());
        REQUIRE(rig.pool.stats().blocksUsed == 0);
    }
    rig.engine.setPreviewSource(&rig.src);
    rig.src.add("/sdcard/a.wav", 20000, 1, 8000);
    REQUIRE(rig.engine.previewStart("/sdcard/a.wav"));
    REQUIRE(rig.run(4) > 0.0f);
}

TEST_CASE("Audition whose file never delivers a frame gives up", "[samplerengine][preview][fail]") {
    Rig rig;
    rig.src.add("/sdcard/a.wav", 20000, 1, 8000);
    rig.src.stall = true;                      // the card is gone from the start
    REQUIRE(rig.engine.previewStart("/sdcard/a.wav"));
    for (uint32_t i = 0; i < SamplerEngine::kPreviewPrimeMaxPasses + 2; ++i) rig.engine.serviceIo();
    REQUIRE_FALSE(rig.engine.previewBusy());   // not Priming for ever
    REQUIRE(rig.pool.stats().blocksUsed == 0);
    rig.src.stall = false;
    REQUIRE(rig.engine.previewStart("/sdcard/a.wav"));
    REQUIRE(rig.run(4) > 0.0f);
}

TEST_CASE("Audition whose card goes away mid-file ends after a bound", "[samplerengine][preview][fail]") {
    Rig rig;
    rig.src.add("/sdcard/a.wav", 1000000, 1, 8000);
    REQUIRE(rig.engine.previewStart("/sdcard/a.wav"));
    REQUIRE(rig.run(4) > 0.0f);
    rig.src.stall = true;                      // and never comes back
    /* 2 s of gap at 64 frames a block, and the ring's lookahead before it. */
    const uint32_t blocks = SamplerEngine::kPreviewGapMaxMs * 44100 / 1000 / 64 + 200;
    rig.run(blocks);
    REQUIRE_FALSE(rig.engine.previewBusy());
    REQUIRE(rig.pool.stats().blocksUsed == 0);
}

TEST_CASE("Audition stopped in a gap ends at once", "[samplerengine][preview][fail]") {
    Rig rig;
    rig.src.add("/sdcard/a.wav", 1000000, 1, 8000);
    REQUIRE(rig.engine.previewStart("/sdcard/a.wav"));
    REQUIRE(rig.run(4) > 0.0f);
    rig.src.stall = true;
    rig.run(80);                               // the ring has run dry: a gap
    REQUIRE(rig.engine.previewBusy());
    /* The fade is the render's, and a voice with no frame to fade never
     * reached the end of it: the stop must not wait on the card. */
    rig.engine.previewStop();
    rig.run(3);
    REQUIRE_FALSE(rig.engine.previewBusy());
    REQUIRE(rig.pool.stats().blocksUsed == 0);
}

TEST_CASE("Audition replaced in a gap gives way to the next one", "[samplerengine][preview][fail]") {
    Rig rig;
    rig.src.add("/sdcard/a.wav", 1000000, 1, 8000);
    rig.src.add("/sdcard/b.wav", 100000, 1, 2000);
    REQUIRE(rig.engine.previewStart("/sdcard/a.wav"));
    rig.run(4);
    rig.src.stall = true;
    rig.run(80);
    REQUIRE(rig.engine.previewStart("/sdcard/b.wav"));
    rig.run(3);                                // a ends at once
    rig.src.stall = false;
    rig.run(6);
    REQUIRE(std::fabs(rig.lastL - kLevel(2000)) < 1e-3f);
}

TEST_CASE("Audition stopped while it is still priming never sounds", "[samplerengine][preview][fail]") {
    Rig rig;
    rig.src.add("/sdcard/a.wav", 20000, 1, 8000);
    rig.src.stall = true;
    REQUIRE(rig.engine.previewStart("/sdcard/a.wav"));
    rig.run(2);                                // ring open, nothing in it
    REQUIRE(rig.engine.previewBusy());
    REQUIRE(rig.pool.stats().blocksUsed > 0);
    rig.engine.previewStop();
    rig.src.stall = false;
    REQUIRE(rig.run(4) == 0.0f);
    REQUIRE_FALSE(rig.engine.previewBusy());
    REQUIRE(rig.pool.stats().blocksUsed == 0);
}

TEST_CASE("Audition: a second one while the first is priming takes its place", "[samplerengine][preview][fail]") {
    Rig rig;
    rig.src.add("/sdcard/a.wav", 20000, 1, 8000);
    rig.src.add("/sdcard/b.wav", 20000, 1, 2000);
    rig.src.stall = true;
    REQUIRE(rig.engine.previewStart("/sdcard/a.wav"));
    rig.run(2);                                // a is priming
    REQUIRE(rig.engine.previewStart("/sdcard/b.wav"));
    rig.src.stall = false;
    rig.run(6);
    REQUIRE(std::fabs(rig.lastL - kLevel(2000)) < 1e-3f);
    REQUIRE(rig.pool.stats().blocksUsed <= crosspad::StreamWindow::kDefaultBlocks);
}

TEST_CASE("Audition asked for before the loader took the engine is dropped", "[samplerengine][preview][fail]") {
    Rig rig;
    rig.src.add("/sdcard/a.wav", 20000, 1, 8000);
    REQUIRE(rig.engine.previewStart("/sdcard/a.wav"));
    rig.engine.beginMutation();                // a kit swap starts before audio_bg saw it
    rig.run(3);
    REQUIRE(rig.pool.stats().blocksUsed == 0); // no ring opened in a pool on its way out
    REQUIRE_FALSE(rig.engine.previewBusy());
    rig.engine.endMutation();
    REQUIRE(rig.run(3) == 0.0f);               // and it does not come back afterwards
    REQUIRE(rig.engine.previewStart("/sdcard/a.wav"));
    REQUIRE(rig.run(4) > 0.0f);
}

TEST_CASE("Audition and pads: a failed audition leaves the kit playing", "[samplerengine][preview][fail]") {
    Rig rig;
    rig.kitSrc.add("kick.wav", 500, 1, 8000);
    crosspad::SamplerKit kit;
    const uint16_t slot = rig.store.load("kick.wav", 1024);
    REQUIRE(slot != SampleStore::kNoSlot);
    kit.pad[0].layers[0].slot = slot;
    kit.pad[0].layerCount = 1;
    kit.pad[0].gain = 1.0f;
    rig.engine.setKit(&kit);

    REQUIRE(rig.engine.previewStart("/sdcard/missing.wav"));
    rig.run(2);
    rig.engine.noteOn(0, 127);
    float kitPeak = 0.0f;
    rig.run(2, &kitPeak);
    REQUIRE(kitPeak > 0.0f);
    REQUIRE_FALSE(rig.engine.previewBusy());
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tearing the engine down while other threads still reach into it: the
// streaming pass that has more than one caller (audio_bg, and a teardown
// waiting for the voices to end), and voices whose render never comes back
// to end them (the render parked -- the USB audio profile, SysEx 1D 06 00).
#include <catch2/catch_test_macros.hpp>

#include "crosspad/sampler/SamplerEngine.hpp"

#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

using crosspad::BlockPool;
using crosspad::ISampleSource;
using crosspad::KitPad;
using crosspad::PadMode;
using crosspad::SampleDesc;
using crosspad::SamplerEngine;
using crosspad::SamplerKit;
using crosspad::SampleStore;

namespace {

void* heapAlloc(size_t n, void*) { return std::malloc(n); }
void  heapFree(void* p, void*)   { std::free(p); }

/// In-memory files. A hook runs inside read(): what a card read that takes
/// long enough for another thread to come in looks like, single-threaded.
class HookSource : public ISampleSource {
public:
    void add(const std::string& path, uint32_t frames, uint8_t ch) {
        files_[path] = File{std::vector<int16_t>(static_cast<size_t>(frames) * ch, 6000), ch};
    }
    bool describe(const char* path, SampleDesc& o) override {
        auto it = files_.find(path);
        if (it == files_.end()) return false;
        o.frames = static_cast<uint32_t>(it->second.pcm.size() / it->second.ch);
        o.sampleRate = 44100;
        o.channels = it->second.ch;
        o.bits = 16;
        return true;
    }
    uint32_t read(const char* path, uint32_t first, uint32_t n, int16_t* dst) override {
        if (onRead) onRead();
        auto it = files_.find(path);
        if (it == files_.end()) return 0;
        const File& f = it->second;
        const uint32_t total = static_cast<uint32_t>(f.pcm.size() / f.ch);
        if (first >= total) return 0;
        if (first + n > total) n = total - first;
        std::memcpy(dst, f.pcm.data() + static_cast<size_t>(first) * f.ch,
                    static_cast<size_t>(n) * f.ch * sizeof(int16_t));
        return n;
    }
    std::function<void()> onRead;

private:
    struct File { std::vector<int16_t> pcm; uint8_t ch = 1; };
    std::map<std::string, File> files_;
};

struct Rig {
    HookSource    src;
    HookSource    prevSrc;
    BlockPool     pool;
    SampleStore   store;
    SamplerEngine engine;
    SamplerKit    kit;

    Rig() {
        BlockPool::Config pc;
        pc.blockBytes = 2048;
        pc.pageBytes  = 2048 * 8;
        pc.maxPages   = 16;
        pc.alloc = &heapAlloc;
        pc.free  = &heapFree;
        REQUIRE(pool.configure(pc));
        REQUIRE(pool.reserve(64) > 0);
        store.bind(&src, &heapAlloc, &heapFree, nullptr);
        engine.bind(&pool, &store);
        engine.setPreviewSource(&prevSrc);
        engine.onPrepare(64, 44100);
        SamplerEngine::Params p;
        p.maxStreamingVoices = 16;
        p.silenceMs = 0.0f;
        engine.setParams(p);
    }

    /// Pad @p pad plays a sample far longer than its 256-frame head: every
    /// voice of it streams.
    void streamingPad(uint8_t pad, const char* path, PadMode mode = PadMode::OneShot) {
        src.add(path, 60000, 1);
        const uint16_t slot = store.load(path, 256);
        REQUIRE(slot != SampleStore::kNoSlot);
        KitPad& kp = kit.pad[pad];
        kp.layers[0].slot = slot;
        kp.layerCount = 1;
        kp.mode = mode;
        kp.gain = 1.0f;
    }

    void block() {
        std::vector<float> out(128, 0.0f);
        engine.mix(out.data(), 64);
        engine.mixPreview(out.data(), 64);
        engine.serviceIo();
    }
};

} // namespace

TEST_CASE("A streaming pass that comes in while another is in the voices is turned away",
          "[samplerengine][teardown]") {
    Rig rig;
    rig.streamingPad(0, "long.wav");
    rig.engine.setKit(&rig.kit);
    rig.engine.noteOn(0, 100);

    /* audio_bg is inside a card read for a voice; the teardown's own wait
     * calls serviceIo() meanwhile. The two must not both walk the voices --
     * one closing a window the other is filling. */
    int inner = -1;
    bool idleInside = true;
    bool once = false;
    rig.src.onRead = [&] {
        if (once) return;
        once = true;
        inner = rig.engine.serviceIo() ? 1 : 0;
        idleInside = rig.engine.ioIdle();
    };
    REQUIRE(rig.engine.serviceIo());              // the pass that opens and fills the ring
    CHECK(inner == 0);                            // the one that came in was refused
    CHECK_FALSE(idleInside);                      // and a teardown can see the pass is running
    CHECK(rig.engine.ioIdle());                   // nothing in it once the pass is out
}

TEST_CASE("abandonVoices is refused while a streaming pass is in the voices",
          "[samplerengine][teardown]") {
    Rig rig;
    rig.streamingPad(0, "long.wav");
    rig.engine.setKit(&rig.kit);
    rig.engine.noteOn(0, 100);

    int inner = -1;
    bool once = false;
    rig.src.onRead = [&] {
        if (once) return;
        once = true;
        inner = rig.engine.abandonVoices() ? 1 : 0;
    };
    rig.engine.serviceIo();
    CHECK(inner == 0);
    CHECK(rig.engine.anySounding());              // nothing was taken from under the pass
}

TEST_CASE("abandonVoices gives every block back exactly once", "[samplerengine][teardown]") {
    Rig rig;
    for (uint8_t p = 0; p < 6; ++p) {
        rig.streamingPad(p, ("s" + std::to_string(p) + ".wav").c_str());
    }
    rig.engine.setKit(&rig.kit);
    for (uint8_t p = 0; p < 6; ++p) rig.engine.noteOn(p, 100);
    rig.block();                                  // every ring open and fed
    rig.block();
    const uint32_t ringsOut = rig.pool.stats().blocksUsed;
    REQUIRE(ringsOut > 0);

    /* One voice ends in the render (Finished, its ring not yet given back),
     * the rest are still Playing: both kinds have to come back, and the
     * Finished one must not come back twice when the next pass runs. */
    rig.engine.stopPad(5);
    for (int i = 0; i < 4; ++i) {
        std::vector<float> out(128, 0.0f);
        rig.engine.mix(out.data(), 64);          // the fade ends it; no serviceIo()
    }
    REQUIRE_FALSE(rig.engine.padSounding(5));
    REQUIRE(rig.pool.stats().blocksUsed == ringsOut);   // still held by the Finished voice

    rig.engine.beginMutation();
    REQUIRE(rig.engine.abandonVoices());
    CHECK(rig.pool.stats().blocksUsed == 0);
    CHECK_FALSE(rig.engine.anySounding());
    CHECK(rig.engine.voicesActive() == 0);
    for (uint8_t p = 0; p < 6; ++p) CHECK_FALSE(rig.engine.padSounding(p));

    rig.engine.serviceIo();                       // a Finished voice would give its ring back again here
    rig.engine.serviceIo();
    CHECK(rig.pool.stats().blocksUsed == 0);      // and the count would wrap under zero
    CHECK(rig.pool.releaseAll());                 // every page is empty: nothing still points in
    rig.engine.endMutation();
}

TEST_CASE("Voices left with the render parked are abandoned and the pool comes back empty",
          "[samplerengine][teardown]") {
    Rig rig;
    rig.streamingPad(0, "loop.wav", PadMode::Loop);
    rig.streamingPad(1, "hold.wav", PadMode::Hold);
    rig.streamingPad(2, "shot.wav");
    rig.prevSrc.add("/sdcard/audition.wav", 40000, 2);
    rig.engine.setKit(&rig.kit);
    for (uint8_t p = 0; p < 3; ++p) rig.engine.noteOn(p, 100);
    REQUIRE(rig.engine.previewStart("/sdcard/audition.wav"));
    rig.block();
    rig.block();
    rig.block();
    REQUIRE(rig.pool.stats().blocksUsed > 0);

    /* The render stops here (AudioTasks_SetSuspended): a fade, a release or
     * the end of a sample is the render's to play, so nothing ends any more,
     * however long the teardown waits and services. */
    rig.engine.beginMutation();
    rig.engine.allNotesOff();
    rig.engine.previewStop();
    for (int i = 0; i < 50; ++i) rig.engine.serviceIo();
    REQUIRE(rig.engine.anySounding());            // stuck, which is the finding
    REQUIRE(rig.engine.previewBusy());
    CHECK(rig.engine.previewState() == 2);        // Playing, for good: the fade is the render's

    REQUIRE(rig.engine.abandonVoices());
    REQUIRE(rig.engine.previewAbandon());
    CHECK_FALSE(rig.engine.anySounding());
    CHECK_FALSE(rig.engine.previewBusy());
    CHECK(rig.engine.previewState() == 0);
    CHECK(rig.engine.ioIdle());
    CHECK(rig.pool.stats().blocksUsed == 0);
    rig.engine.setKit(nullptr);
    rig.store.unload();
    CHECK(rig.pool.releaseAll());
    rig.engine.endMutation();

    /* And the engine is usable again: the next kit plays. */
    REQUIRE(rig.pool.reserve(64) > 0);
    rig.streamingPad(3, "next.wav");
    rig.engine.setKit(&rig.kit);
    rig.engine.noteOn(3, 100);
    rig.block();
    CHECK(rig.engine.padSounding(3));
}

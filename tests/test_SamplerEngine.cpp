// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "crosspad/sampler/SamplerEngine.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
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

/// A bank of in-memory "files". Counts reads so a test can prove that a
/// resident sample never goes back to the card.
class MemSource : public ISampleSource {
public:
    void add(const std::string& path, uint32_t frames, uint8_t ch, uint32_t rate = 44100) {
        File f;
        f.ch = ch;
        f.rate = rate;
        f.pcm.resize(static_cast<size_t>(frames) * ch);
        for (uint32_t i = 0; i < frames; ++i) {
            for (uint8_t c = 0; c < ch; ++c) {
                // A full-scale square-ish ramp, distinct per frame and channel.
                f.pcm[i * ch + c] = static_cast<int16_t>(8000 + (i % 100) * 10 + c);
            }
        }
        files_[path] = std::move(f);
    }

    bool describe(const char* path, SampleDesc& o) override {
        auto it = files_.find(path);
        if (it == files_.end()) return false;
        o.frames = static_cast<uint32_t>(it->second.pcm.size() / it->second.ch);
        o.sampleRate = it->second.rate;
        o.channels = it->second.ch;
        o.bits = 16;
        return true;
    }

    uint32_t read(const char* path, uint32_t first, uint32_t n, int16_t* dst) override {
        auto it = files_.find(path);
        if (it == files_.end()) return 0;
        const File& f = it->second;
        const uint32_t total = static_cast<uint32_t>(f.pcm.size() / f.ch);
        if (first >= total) return 0;
        if (first + n > total) n = total - first;
        std::memcpy(dst, f.pcm.data() + static_cast<size_t>(first) * f.ch,
                    static_cast<size_t>(n) * f.ch * sizeof(int16_t));
        ++reads;
        framesRead += n;
        return n;
    }

    uint32_t reads = 0;
    uint32_t framesRead = 0;

private:
    struct File { std::vector<int16_t> pcm; uint8_t ch = 1; uint32_t rate = 44100; };
    std::map<std::string, File> files_;
};

struct Rig {
    MemSource    src;
    BlockPool    pool;
    SampleStore  store;
    SamplerEngine engine;
    SamplerKit   kit;

    void build(uint32_t headFrames = 1024, uint32_t maxPages = 16) {
        BlockPool::Config pc;
        pc.blockBytes = 2048;
        pc.pageBytes  = 16384;
        pc.maxPages   = maxPages;
        pc.alloc = &heapAlloc;
        pc.free  = &heapFree;
        REQUIRE(pool.configure(pc));
        REQUIRE(pool.reserve(64) > 0);
        store.bind(&src, &heapAlloc, &heapFree, nullptr);
        engine.bind(&pool, &store);
        engine.onPrepare(64, 44100);
        (void)headFrames;
    }

    /// Put @p path on @p pad, loading it with the given head.
    void assign(uint8_t pad, const char* path, uint32_t headFrames, PadMode mode = PadMode::OneShot) {
        const uint16_t slot = store.load(path, headFrames);
        REQUIRE(slot != SampleStore::kNoSlot);
        KitPad& kp = kit.pad[pad];
        kp.layers[0].slot = slot;
        kp.layers[0].start = 0;
        kp.layers[0].end = 0;
        kp.layerCount = 1;
        kp.mode = mode;
        kp.gain = 1.0f;
        kp.pan = 0.0f;
    }

    void publish() { engine.setKit(&kit); }

    /// Render n blocks of 64 frames, servicing the stream between them as the
    /// BG task would, and return the peak of what came out.
    float run(uint32_t blocks, uint32_t frames = 64) {
        std::vector<float> out(frames * 2);
        float peak = 0.0f;
        for (uint32_t b = 0; b < blocks; ++b) {
            std::fill(out.begin(), out.end(), 0.0f);
            engine.mix(out.data(), frames);
            for (float s : out) peak = std::max(peak, std::fabs(s));
            engine.serviceIo();
        }
        return peak;
    }
};

} // namespace

TEST_CASE("SamplerEngine sounds in the same block the note arrives in", "[samplerengine]") {
    Rig rig;
    rig.build();
    rig.src.add("kick.wav", 500, 1);          // shorter than the head: resident whole
    rig.assign(0, "kick.wav", 1024);
    rig.publish();

    const uint32_t readsAfterLoad = rig.src.reads;

    std::vector<float> out(64 * 2, 0.0f);
    rig.engine.noteOn(0, 100);
    rig.engine.mix(out.data(), 64);

    float peak = 0.0f;
    for (float s : out) peak = std::max(peak, std::fabs(s));
    REQUIRE(peak > 0.0f);                      // it is already making sound
    // And it did it without going anywhere near the source.
    REQUIRE(rig.src.reads == readsAfterLoad);
    REQUIRE(rig.engine.counters().notesStarted == 1);
}

TEST_CASE("SamplerEngine lets a one-shot end by itself and gives the voice back", "[samplerengine]") {
    Rig rig;
    rig.build();
    rig.src.add("hat.wav", 300, 1);
    rig.assign(0, "hat.wav", 1024);
    rig.publish();

    rig.engine.noteOn(0, 127);
    REQUIRE(rig.run(2) > 0.0f);                // 128 frames: still inside 300
    REQUIRE(rig.engine.padSounding(0));

    rig.run(10);                               // well past the end
    REQUIRE_FALSE(rig.engine.padSounding(0));
    REQUIRE_FALSE(rig.engine.anySounding());
    REQUIRE(rig.engine.voicesActive() == 0);
    REQUIRE(rig.engine.counters().underruns == 0);
}

TEST_CASE("SamplerEngine keeps a loop going and Hold stops on release", "[samplerengine]") {
    Rig rig;
    rig.build();
    rig.src.add("loop.wav", 400, 1);
    rig.src.add("pad.wav", 400, 1);
    rig.assign(0, "loop.wav", 1024, PadMode::Loop);
    rig.assign(1, "pad.wav", 1024, PadMode::Hold);
    rig.publish();

    rig.engine.noteOn(0, 127);
    rig.run(40);                               // 2560 frames over a 400-frame loop
    REQUIRE(rig.engine.padSounding(0));        // a loop does not stop on its own
    rig.engine.noteOff(0);                     // and ignores a release
    rig.run(10);
    REQUIRE(rig.engine.padSounding(0));

    rig.engine.noteOn(1, 127);
    rig.run(2);
    REQUIRE(rig.engine.padSounding(1));
    rig.engine.noteOff(1);
    rig.run(40);                               // the release fade is 8 ms = 353 frames
    REQUIRE_FALSE(rig.engine.padSounding(1));

    rig.engine.allNotesOff();
    rig.run(20);
    REQUIRE_FALSE(rig.engine.anySounding());
}

TEST_CASE("SamplerEngine chokes within a group and not across groups", "[samplerengine]") {
    Rig rig;
    rig.build();
    rig.src.add("open.wav", 4000, 1);
    rig.src.add("closed.wav", 4000, 1);
    rig.src.add("kick.wav", 4000, 1);
    rig.assign(0, "open.wav", 8000);
    rig.assign(1, "closed.wav", 8000);
    rig.assign(2, "kick.wav", 8000);
    rig.kit.pad[0].choke = 1;
    rig.kit.pad[1].choke = 1;
    rig.kit.pad[2].choke = 0;
    rig.publish();

    rig.engine.noteOn(0, 127);
    rig.engine.noteOn(2, 127);
    rig.run(2);
    REQUIRE(rig.engine.padSounding(0));
    REQUIRE(rig.engine.padSounding(2));

    rig.engine.noteOn(1, 127);                 // same group as pad 0
    rig.run(10);                               // the choke fade is 2 ms = 88 frames
    REQUIRE_FALSE(rig.engine.padSounding(0));  // cut
    REQUIRE(rig.engine.padSounding(1));        // the one that cut it
    REQUIRE(rig.engine.padSounding(2));        // a different group, untouched
}

TEST_CASE("SamplerEngine steals the oldest voice above the cap, and never drops", "[samplerengine]") {
    Rig rig;
    rig.build();
    for (int p = 0; p < 16; ++p) {
        rig.src.add("s" + std::to_string(p) + ".wav", 40000, 1);
        rig.assign(static_cast<uint8_t>(p), ("s" + std::to_string(p) + ".wav").c_str(), 60000);
    }
    SamplerEngine::Params pr;
    pr.maxVoices = 8;
    rig.engine.setParams(pr);
    rig.publish();

    for (int p = 0; p < 16; ++p) {
        rig.engine.noteOn(static_cast<uint8_t>(p), 127);
        rig.run(1);
    }
    const auto c = rig.engine.counters();
    REQUIRE(c.notesStarted == 16);
    REQUIRE(c.notesDropped == 0);              // the fade slots guarantee a free one
    REQUIRE(c.steals > 0);
    rig.run(20);
    REQUIRE(rig.engine.voicesActive() <= pr.maxVoices);
}

TEST_CASE("SamplerEngine streams past the head without an underrun", "[samplerengine]") {
    Rig rig;
    rig.build();
    // 60 000 frames against a 1024-frame head: almost all of it is streamed.
    rig.src.add("long.wav", 60000, 1);
    rig.assign(0, "long.wav", 1024);
    rig.publish();

    rig.engine.noteOn(0, 127);
    // 60000 frames is 938 blocks of 64.
    const float peak = rig.run(1000);
    REQUIRE(peak > 0.0f);
    const auto c = rig.engine.counters();
    REQUIRE(c.underruns == 0);
    REQUIRE(c.streamRefused == 0);
    REQUIRE_FALSE(rig.engine.anySounding());
    // And every block the voice held is back in the pool.
    rig.engine.serviceIo();
    REQUIRE(rig.pool.stats().blocksUsed == 0);
}

TEST_CASE("SamplerEngine plays a 22 kHz file at the right speed", "[samplerengine]") {
    Rig rig;
    rig.build();
    // Same number of frames, half the rate: it must last twice as long.
    rig.src.add("half.wav", 2000, 1, 22050);
    rig.assign(0, "half.wav", 4000);
    rig.publish();

    rig.engine.noteOn(0, 127);
    // At rate 22050 into a 44100 engine the increment is 0.5 frames per output
    // frame, so 2000 source frames take 4000 output frames = 62.5 blocks.
    rig.run(50);
    REQUIRE(rig.engine.padSounding(0));        // not finished at 3200 frames
    rig.run(20);
    REQUIRE_FALSE(rig.engine.padSounding(0));  // finished by 4480
}

TEST_CASE("SamplerEngine transposes a pad in real time", "[samplerengine]") {
    Rig rig;
    rig.build();
    rig.src.add("note.wav", 4800, 1);
    rig.assign(0, "note.wav", 8000);
    rig.assign(1, "note.wav", 8000);
    rig.kit.pad[1].semitones = 12;             // an octave up: twice the speed
    rig.publish();

    rig.engine.noteOn(1, 127);
    rig.run(38);                               // 2432 output frames = 4864 source: past the end
    REQUIRE_FALSE(rig.engine.padSounding(1));

    rig.engine.noteOn(0, 127);                 // untransposed: still going at 2432
    rig.run(38);
    REQUIRE(rig.engine.padSounding(0));
}

TEST_CASE("SamplerEngine does nothing while the loader has the kit", "[samplerengine]") {
    Rig rig;
    rig.build();
    rig.src.add("x.wav", 2000, 1);
    rig.assign(0, "x.wav", 4000);
    rig.publish();

    rig.engine.beginMutation();
    rig.engine.noteOn(0, 127);
    REQUIRE(rig.engine.counters().notesStarted == 0);
    rig.run(2);
    REQUIRE_FALSE(rig.engine.anySounding());

    rig.engine.endMutation();
    rig.engine.noteOn(0, 127);
    REQUIRE(rig.engine.counters().notesStarted == 1);
}

TEST_CASE("SamplerEngine plays its head when the pool has no blocks", "[samplerengine]") {
    Rig rig;
    // One page of eight blocks, and a voice wants eight: the second voice gets
    // nothing to stream through.
    rig.build(1024, 1);
    rig.src.add("a.wav", 40000, 1);
    rig.src.add("b.wav", 40000, 1);
    rig.assign(0, "a.wav", 1024);
    rig.assign(1, "b.wav", 1024);
    rig.publish();

    rig.engine.noteOn(0, 127);
    rig.engine.noteOn(1, 127);
    REQUIRE(rig.engine.counters().streamRefused >= 1);
    // The refused one still sounds -- it has its head -- and then stops
    // cleanly rather than reading something that is not its own sample.
    REQUIRE(rig.run(4) > 0.0f);
    rig.run(40);
    REQUIRE_FALSE(rig.engine.padSounding(1));
}

TEST_CASE("SamplerEngine reports the pad and layer it is actually playing", "[samplerengine]") {
    static std::vector<std::pair<uint8_t, uint8_t>> started, ended;
    started.clear();
    ended.clear();

    Rig rig;
    rig.build();
    rig.src.add("soft.wav", 300, 1);
    rig.src.add("hard.wav", 300, 1);
    const uint16_t s0 = rig.store.load("soft.wav", 1024);
    const uint16_t s1 = rig.store.load("hard.wav", 1024);
    KitPad& kp = rig.kit.pad[0];
    kp.layers[0].slot = s0;
    kp.layers[1].slot = s1;
    kp.layerCount = 2;
    kp.gain = 1.0f;
    rig.publish();

    rig.engine.setNoteCallbacks(
        [](uint8_t p, uint8_t l, void*) { started.push_back({p, l}); },
        [](uint8_t p, uint8_t l, void*) { ended.push_back({p, l}); },
        nullptr);

    rig.engine.noteOn(0, 30);                  // low half -> layer 0
    rig.run(1);
    REQUIRE(started.size() == 1);
    REQUIRE(started[0] == std::make_pair<uint8_t, uint8_t>(0, 0));

    rig.engine.noteOn(0, 120);                 // high half -> layer 1
    rig.run(1);
    REQUIRE(started.size() == 2);
    REQUIRE(started[1].second == 1);

    rig.run(20);
    REQUIRE(ended.size() == 2);
}

TEST_CASE("SamplerEngine gives the pool back once a kit stops sounding", "[samplerengine]") {
    Rig rig;
    rig.build();
    for (int p = 0; p < 8; ++p) {
        rig.src.add("s" + std::to_string(p) + ".wav", 30000, 1);
        rig.assign(static_cast<uint8_t>(p), ("s" + std::to_string(p) + ".wav").c_str(), 1024);
    }
    rig.publish();

    REQUIRE(rig.pool.stats().blocksUsed == 0);
    for (int p = 0; p < 8; ++p) rig.engine.noteOn(static_cast<uint8_t>(p), 127);
    REQUIRE(rig.pool.stats().blocksUsed > 0);

    rig.engine.allNotesOff();
    rig.run(40);
    rig.engine.serviceIo();
    REQUIRE(rig.pool.stats().blocksUsed == 0);
    // And with nothing in them, every page goes back to the heap.
    REQUIRE(rig.pool.trim() > 0);
    REQUIRE(rig.pool.stats().pages == 0);
}

TEST_CASE("SamplerEngine stops a one-shot once it has gone inaudible", "[samplerengine]") {
    Rig rig;
    rig.build();
    // A long file whose first 2000 frames sound and whose tail is silence --
    // which is what a drum hit's file is.
    rig.src.add("hit.wav", 60000, 1);
    const uint16_t slot = rig.store.load("hit.wav", 60000);
    REQUIRE(slot != SampleStore::kNoSlot);
    // Silence everything past 2000 frames, in the store's resident head.
    auto* head = const_cast<int16_t*>(rig.store.slot(slot)->head);
    for (uint32_t i = 2000; i < 60000; ++i) head[i] = 0;

    KitPad& kp = rig.kit.pad[0];
    kp.layers[0].slot = slot;
    kp.layerCount = 1;
    kp.gain = 1.0f;
    rig.publish();

    SamplerEngine::Params pr;
    pr.silenceMs = 20.0f;              // 882 frames
    rig.engine.setParams(pr);

    rig.engine.noteOn(0, 127);
    rig.run(20);                       // 1280 frames: still in the sounding part
    REQUIRE(rig.engine.padSounding(0));

    rig.run(40);                       // past 2000 + 882
    REQUIRE_FALSE(rig.engine.padSounding(0));
    REQUIRE(rig.engine.counters().silenced == 1);
    // And it stopped long before the file ended.
    REQUIRE(rig.engine.counters().notesStarted == 1);
}

TEST_CASE("SamplerEngine lets a loop run through its quiet part", "[samplerengine]") {
    Rig rig;
    rig.build();
    rig.src.add("loop.wav", 8000, 1);
    const uint16_t slot = rig.store.load("loop.wav", 8000);
    auto* head = const_cast<int16_t*>(rig.store.slot(slot)->head);
    // A loop with a long silent stretch in the middle -- a bar of rest.
    for (uint32_t i = 1000; i < 7000; ++i) head[i] = 0;

    KitPad& kp = rig.kit.pad[0];
    kp.layers[0].slot = slot;
    kp.layerCount = 1;
    kp.gain = 1.0f;
    kp.mode = PadMode::Loop;
    rig.publish();

    SamplerEngine::Params pr;
    pr.silenceMs = 20.0f;
    rig.engine.setParams(pr);

    rig.engine.noteOn(0, 127);
    rig.run(100);                      // 6400 frames, most of them silent
    REQUIRE(rig.engine.padSounding(0));          // a loop is never cut short
    REQUIRE(rig.engine.counters().silenced == 0);
    rig.engine.allNotesOff();
    rig.run(20);
}

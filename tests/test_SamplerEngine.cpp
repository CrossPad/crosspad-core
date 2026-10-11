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

    /// Zero frames [from, to) of @p path -- a rest, or a silent tail. Before
    /// the store loads it, so the head has it too.
    void silence(const std::string& path, uint32_t from, uint32_t to) {
        File& f = files_[path];
        for (size_t i = static_cast<size_t>(from) * f.ch; i < static_cast<size_t>(to) * f.ch && i < f.pcm.size(); ++i) {
            f.pcm[i] = 0;
        }
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

    /// @p blocksPerPage lets a test size the pool in *rings* rather than in
    /// blocks, so it keeps meaning what it meant when StreamWindow's ring
    /// depth changes (it went from eight blocks to four when the block got
    /// four times bigger).
    void build(uint32_t headFrames = 1024, uint32_t maxPages = 16,
               uint32_t blocksPerPage = 8) {
        BlockPool::Config pc;
        pc.blockBytes = 2048;
        pc.pageBytes  = 2048 * blocksPerPage;
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
    /* One page holding exactly one ring, and one page only: the second voice
     * gets nothing to stream through. Written against kDefaultBlocks so it
     * stays true if the ring depth changes again. */
    rig.build(1024, 1, crosspad::StreamWindow::kDefaultBlocks);
    rig.src.add("a.wav", 40000, 1);
    rig.src.add("b.wav", 40000, 1);
    rig.assign(0, "a.wav", 1024);
    rig.assign(1, "b.wav", 1024);
    rig.publish();

    rig.engine.noteOn(0, 127);
    rig.engine.noteOn(1, 127);
    /* The note itself takes nothing from the pool -- the ring is opened by the
     * streaming thread on its next pass, which is where the refusal shows. */
    REQUIRE(rig.engine.counters().streamRefused == 0);
    rig.engine.serviceIo();
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
    // Still nothing: striking a pad does not touch the pool.
    REQUIRE(rig.pool.stats().blocksUsed == 0);
    rig.engine.serviceIo();
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

TEST_CASE("SamplerEngine plays a one-shot on through a rest inside it", "[samplerengine]") {
    // A phrase, not a hit: sound, a rest, sound again, then the tail. Waiting
    // 200 ms took a 221 ms rest for the end of the file and cut the phrase
    // there (DREAMSCAPE pad 4, 09.10.2026).
    Rig rig;
    rig.build();
    rig.src.add("phrase.wav", 40000, 1);
    rig.src.silence("phrase.wav", 2000, 15230);      // a 300 ms rest
    rig.src.silence("phrase.wav", 18000, 40000);     // the tail
    rig.assign(0, "phrase.wav", 40000);
    rig.publish();                     // the default wait

    rig.engine.noteOn(0, 127);
    rig.run(240);                      // 15360 frames: through the rest
    REQUIRE(rig.engine.padSounding(0));
    REQUIRE(rig.run(40) > 0.1f);       // to 17920: the sound after it
    REQUIRE(rig.engine.counters().silenced == 0);
}

TEST_CASE("SamplerEngine stops the next hit where the first found the sound ends", "[samplerengine]") {
    // Streamed: only the head is resident, so where the sound ends is not
    // known until a voice has played past it.
    Rig rig;
    rig.build();
    rig.src.add("hit.wav", 20000, 1);
    rig.src.silence("hit.wav", 3000, 20000);
    rig.assign(0, "hit.wav", 1024);
    rig.publish();
    const uint16_t slot = rig.kit.pad[0].layers[0].slot;
    REQUIRE(rig.store.audibleEnd(slot) == 0);

    SamplerEngine::Params pr;
    pr.silenceMs = 20.0f;              // 882 frames
    rig.engine.setParams(pr);

    rig.engine.noteOn(0, 127);
    rig.run(80);                       // 5120 frames: past 3000 + 882
    REQUIRE_FALSE(rig.engine.padSounding(0));
    REQUIRE(rig.engine.counters().silenced == 1);
    REQUIRE(rig.store.audibleEnd(slot) == 3000);

    // Softly this time: the end is the file's, not this hit's level.
    rig.engine.noteOn(0, 20);
    rig.run(48);                       // 3072 frames: just past the sound
    REQUIRE_FALSE(rig.engine.padSounding(0));
    REQUIRE(rig.engine.counters().silenced == 1);   // ended at the end, not by waiting

    rig.store.unload();
    REQUIRE(rig.store.audibleEnd(slot) == 0);
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


TEST_CASE("SamplerEngine takes nothing from the pool on the note thread", "[samplerengine]") {
    /* The one thread a player is waiting on must do no work that can be
     * contended. Striking a pad therefore leaves the ring to the streaming
     * thread, and this is the test that says so. */
    Rig rig;
    rig.build();
    rig.src.add("long.wav", 60000, 1);
    rig.assign(0, "long.wav", 1024);
    rig.publish();

    const uint32_t before = rig.pool.stats().blocksUsed;
    rig.engine.noteOn(0, 127);
    REQUIRE(rig.pool.stats().blocksUsed == before);

    // And it sounds straight away regardless -- the head is resident.
    std::vector<float> out(128, 0.0f);
    rig.engine.mix(out.data(), 64);
    float peak = 0.0f;
    for (float v : out) peak = std::max(peak, std::fabs(v));
    REQUIRE(peak > 0.0f);

    // The ring arrives on the next pass, well before the voice needs it.
    rig.engine.serviceIo();
    REQUIRE(rig.pool.stats().blocksUsed > before);
    REQUIRE(rig.run(1000) > 0.0f);
    REQUIRE(rig.engine.counters().underruns == 0);
}

TEST_CASE("SamplerEngine moves a fader on the voice already sounding", "[samplerengine]") {
    /* A level turned over a playing loop has to be heard on that loop, not on
     * the next hit -- that is what the Pad Mixer's fader means. */
    Rig rig;
    rig.build();
    rig.src.add("loop.wav", 4000, 1);
    rig.assign(0, "loop.wav", 8000, PadMode::Loop);
    rig.publish();

    SamplerEngine::Params pr;
    pr.silenceMs = 0.0f;                   // do not cut it while measuring
    pr.attackMs  = 0.0f;                   // full level from the first frame
    rig.engine.setParams(pr);

    rig.engine.noteOn(0, 127);
    rig.run(4);                            // past any rise
    const float loud = rig.run(8);
    REQUIRE(loud > 0.0f);

    // Halve the pad's level while the voice is mid-loop.
    rig.engine.setPadMix(0, 50, 64);       // volume 50 of 100, centre
    const float quiet = rig.run(8);
    REQUIRE(quiet > 0.0f);
    REQUIRE(quiet < loud * 0.75f);         // audibly down, on this voice

    // And panned hard left, the right channel goes quiet while the left does not.
    rig.engine.setPadMix(0, 100, 0);
    std::vector<float> out(64 * 2, 0.0f);
    rig.engine.mix(out.data(), 64);
    float pl = 0.0f, pr2 = 0.0f;
    for (uint32_t i = 0; i < 64; ++i) {
        pl  = std::max(pl,  std::fabs(out[i * 2]));
        pr2 = std::max(pr2, std::fabs(out[i * 2 + 1]));
    }
    REQUIRE(pl > 0.0f);
    REQUIRE(pr2 < pl * 0.2f);

    rig.engine.allNotesOff();
    rig.run(20);
}

TEST_CASE("SamplerEngine stops one pad and leaves the others alone", "[samplerengine]") {
    /* What the pad editor needs before it points a pad at a different file.
     * noteOff() is not it: a one-shot ignores a release and a loop keeps
     * going, which is the whole reason this exists. */
    Rig rig;
    rig.build();
    rig.src.add("loop.wav", 400, 1);
    rig.src.add("hit.wav", 40000, 1);
    rig.assign(0, "loop.wav", 1024, PadMode::Loop);
    rig.assign(1, "hit.wav", 1024, PadMode::OneShot);
    rig.publish();

    rig.engine.noteOn(0, 127);
    rig.engine.noteOn(1, 127);
    rig.run(10);
    REQUIRE(rig.engine.padSounding(0));
    REQUIRE(rig.engine.padSounding(1));

    rig.engine.stopPad(0);
    rig.run(20);                               // the fade is 2 ms = 88 frames
    REQUIRE_FALSE(rig.engine.padSounding(0));
    REQUIRE(rig.engine.padSounding(1));        // the one-shot was not touched

    rig.engine.stopPad(1);
    rig.run(20);
    REQUIRE_FALSE(rig.engine.anySounding());

    rig.engine.stopPad(99);                    // out of range: nothing happens
    REQUIRE_FALSE(rig.engine.anySounding());
}

TEST_CASE("SampleStore::forget drops heads without freeing them", "[samplestore]") {
    /* The arena is one block that gets re-registered. A pointer from before
     * that is not the heap's any more, so handing one back asserts -- which
     * is what unload() would do. forget() is the other half of
     * BlockPool::forgetPages(), and the pair is what makes an arena reset
     * safe from any caller. */
    struct Alloc {
        static void* a(size_t n, void*) { return std::malloc(n); }
        static void  f(void* p, void*)  { ++frees(); std::free(p); }
        static int&  frees() { static int n = 0; return n; }
    };
    Alloc::frees() = 0;

    MemSource src;
    src.add("a.wav", 8000, 1);
    src.add("b.wav", 8000, 2);

    SampleStore store;
    store.bind(&src, &Alloc::a, &Alloc::f, nullptr);
    REQUIRE(store.load("a.wav", 1024) != SampleStore::kNoSlot);
    REQUIRE(store.load("b.wav", 1024) != SampleStore::kNoSlot);
    REQUIRE(store.slotCount() == 2);
    REQUIRE(store.stats().headBytes > 0);

    store.forget();
    REQUIRE(Alloc::frees() == 0);              // nothing was handed back
    REQUIRE(store.slotCount() == 0);
    REQUIRE(store.stats().headBytes == 0);
    REQUIRE(store.slot(0) == nullptr);

    /* And the store is usable again afterwards, against a fresh allocator. */
    REQUIRE(store.load("a.wav", 1024) != SampleStore::kNoSlot);
    store.unload();
    REQUIRE(Alloc::frees() == 1);              // this one *is* handed back
}

TEST_CASE("SamplerEngine retriggers a pad instead of layering on it", "[samplerengine]") {
    /* The prebuilt engine had one player slot per pad, so a second hit
     * restarted that pad. Allocating a voice per note layers unless told
     * otherwise, and four fast hits left four copies sounding -- heard as a
     * pad "playing several times from the beginning". */
    Rig rig;
    rig.build();
    rig.src.add("long.wav", 40000, 1);
    rig.assign(0, "long.wav", 1024, PadMode::OneShot);
    rig.assign(1, "long.wav", 1024, PadMode::OneShot);
    rig.publish();

    for (int i = 0; i < 4; ++i) { rig.engine.noteOn(0, 110); rig.run(2); }
    rig.run(20);                               // past the 2 ms retrigger fade
    REQUIRE(rig.engine.voicesActive() == 1);   // one pad, one voice
    REQUIRE(rig.engine.padSounding(0));

    /* Another pad is untouched by it. */
    rig.engine.noteOn(1, 110);
    rig.run(20);
    REQUIRE(rig.engine.voicesActive() == 2);
    REQUIRE(rig.engine.padSounding(0));
    REQUIRE(rig.engine.padSounding(1));

    /* And a kit that wants its pads to pile up can still say so. */
    auto p = SamplerEngine::Params{};
    p.monoPerPad = false;
    rig.engine.setParams(p);
    for (int i = 0; i < 3; ++i) { rig.engine.noteOn(0, 110); rig.run(2); }
    rig.run(10);
    REQUIRE(rig.engine.voicesActive() > 2);
}

TEST_CASE("SamplerEngine keeps the streaming voices inside their own budget",
          "[samplerengine]") {
    /* A resident voice is arithmetic; a streaming one also has to be fed off
     * the card, and the card gives out first. Measured on a v2: sixteen
     * resident voices fit the block, ten streaming ones do, and from twelve
     * the engine cannot feed them all -- several starve, which is heard as
     * notes cutting out at random. The budget turns that into one deliberate
     * fade of the oldest streaming voice. */
    Rig rig;
    rig.build();
    auto p = SamplerEngine::Params{};
    p.maxVoices = 16;
    p.maxStreamingVoices = 4;        // small, so the test is short
    p.monoPerPad = false;            // one pad per voice is not the point here
    rig.engine.setParams(p);

    /* Eight pads, each a sample far longer than its head: every voice streams. */
    for (int i = 0; i < 8; ++i) {
        const std::string name = "s" + std::to_string(i) + ".wav";
        rig.src.add(name.c_str(), 400000, 1);
        rig.assign(static_cast<uint8_t>(i), name.c_str(), 1024, PadMode::Loop);
    }
    rig.publish();

    for (int i = 0; i < 8; ++i) { rig.engine.noteOn(static_cast<uint8_t>(i), 110); rig.run(2); }
    rig.run(30);                     // past the fades the budget asked for

    const auto m = rig.engine.meter();
    REQUIRE(m.streaming <= 4);                 // the budget held
    /* The limit a meter shows comes from the kit, so it is steady while the
     * kit is loaded rather than flipping back every time the last streaming
     * voice fades -- which is what the first version of this did. */
    REQUIRE(m.limit == 4);
    REQUIRE(rig.engine.counters().steals > 0); // tails were cut, deliberately
    REQUIRE(rig.engine.counters().notesDropped == 0);   // no hit was ever lost

    /* A resident pad is not charged to it. */
    rig.src.add("short.wav", 500, 1);
    rig.assign(9, "short.wav", 1024, PadMode::Loop);
    rig.publish();
    const uint8_t before = rig.engine.meter().streaming;
    rig.engine.noteOn(9, 110);
    rig.run(4);
    REQUIRE(rig.engine.meter().streaming == before);
    REQUIRE(rig.engine.padSounding(9));
    /* And the kit still holds long samples, so the ceiling has not moved. */
    REQUIRE(rig.engine.meter().limit == 4);
}

TEST_CASE("SamplerEngine transposes one note, not the pad", "[samplerengine][levels]") {
    Rig rig;
    rig.build();
    rig.src.add("note.wav", 4800, 1);
    rig.assign(0, "note.wav", 8000);
    rig.publish();

    rig.engine.noteOn(0, 127, 12, 16);         // an octave up for this hit only
    rig.run(38);                               // 4864 source frames: past the end
    REQUIRE_FALSE(rig.engine.padSounding(0));

    rig.engine.noteOn(0, 127);                 // the pad itself is where it was
    rig.run(38);
    REQUIRE(rig.engine.padSounding(0));
}

TEST_CASE("SamplerEngine keeps one pad struck from several keys as a chord", "[samplerengine][levels]") {
    Rig rig;
    rig.build();
    rig.src.add("long.wav", 40000, 1);
    rig.assign(0, "long.wav", 1024, PadMode::Hold);
    rig.publish();

    /* Three pads of 16 levels playing pad 0 at three pitches. */
    rig.engine.noteOn(0, 100, 0, 16);
    rig.engine.noteOn(0, 100, 4, 20);
    rig.engine.noteOn(0, 100, 7, 23);
    rig.run(20);
    REQUIRE(rig.engine.voicesActive() == 3);

    /* The same key again retriggers its own note only. */
    rig.engine.noteOn(0, 100, 4, 20);
    rig.run(20);
    REQUIRE(rig.engine.voicesActive() == 3);

    /* A release lets go of its own note only. */
    rig.engine.noteOff(0, 20);
    rig.run(40);
    REQUIRE(rig.engine.voicesActive() == 2);
    rig.engine.noteOff(0, 16);
    rig.engine.noteOff(0, 23);
    rig.run(40);
    REQUIRE_FALSE(rig.engine.padSounding(0));
}

TEST_CASE("SamplerKit pans as the mixer: unity at the centre", "[samplerengine]") {
    float l = 0, r = 0;
    SamplerKit::panGains(0.0f, l, r);
    REQUIRE(l == 1.0f);
    REQUIRE(r == 1.0f);
    SamplerKit::panGains(1.0f, l, r);
    REQUIRE(l == 0.0f);
    REQUIRE(r == 1.0f);
    SamplerKit::panGains(-0.5f, l, r);
    REQUIRE(l == 1.0f);
    REQUIRE(r == 0.5f);
}

TEST_CASE("SampleStore::unloadSlot gives back one head and keeps the rest", "[samplestore][swap]") {
    /* A kit swapped under a performance retires the outgoing kit's samples
     * one at a time; the incoming kit's heads, loaded beside them, stay. */
    struct Alloc {
        static void* a(size_t n, void*) { return std::malloc(n); }
        static void  f(void* p, void*)  { ++frees(); std::free(p); }
        static int&  frees() { static int n = 0; return n; }
    };
    Alloc::frees() = 0;

    MemSource src;
    src.add("a.wav", 8000, 1);
    src.add("b.wav", 8000, 2);

    SampleStore store;
    store.bind(&src, &Alloc::a, &Alloc::f, nullptr);
    const uint16_t a = store.load("a.wav", 1024);
    const uint16_t b = store.load("b.wav", 1024);
    REQUIRE(a != SampleStore::kNoSlot);
    REQUIRE(b != SampleStore::kNoSlot);
    const size_t both = store.stats().headBytes;

    store.unloadSlot(a);
    REQUIRE(Alloc::frees() == 1);
    REQUIRE(store.slot(a) == nullptr);
    REQUIRE(store.slot(b) != nullptr);
    REQUIRE(store.slotCount() == 1);
    REQUIRE(store.stats().headBytes == both - 1024 * sizeof(int16_t));

    store.unloadSlot(a);                       // already gone: nothing happens
    REQUIRE(Alloc::frees() == 1);

    REQUIRE(store.load("a.wav", 1024) == a);   // the slot is free for the next sample
    store.unload();
    REQUIRE(Alloc::frees() == 3);
}

TEST_CASE("SamplerEngine swaps a pad under the voice it is playing", "[samplerengine][swap]") {
    /* A kit changed while the player keeps playing. The pad is pointed at its
     * new sample at once; the voice already sounding plays the old one to
     * its own end; the old head may go back only after that. */
    Rig rig;
    rig.build();
    rig.src.add("old.wav", 800, 1);            // both resident whole
    rig.src.add("new.wav", 500, 1);
    rig.assign(0, "old.wav", 1024);
    rig.publish();
    const uint16_t oldSlot = rig.kit.pad[0].layers[0].slot;

    rig.engine.noteOn(0, 127);
    rig.run(2);                                // 128 frames in
    REQUIRE(rig.engine.padSounding(0));

    const uint16_t newSlot = rig.store.load("new.wav", 1024);
    REQUIRE(newSlot != SampleStore::kNoSlot);
    rig.kit.pad[0].layers[0].slot = newSlot;   // the swap: one pad entry
    REQUIRE(rig.engine.slotInUse(oldSlot));    // its voice still reads it
    REQUIRE(rig.engine.slotInUse(newSlot));    // the kit names it

    /* 576 frames: past the new sample's 500, short of the old one's 800 --
     * so a voice still sounding is the old sample, uninterrupted. */
    rig.run(7);
    REQUIRE(rig.engine.padSounding(0));
    REQUIRE(rig.engine.slotInUse(oldSlot));

    rig.run(6);                                // past 800: it ended, and was freed
    REQUIRE_FALSE(rig.engine.padSounding(0));
    REQUIRE_FALSE(rig.engine.slotInUse(oldSlot));
    rig.store.unloadSlot(oldSlot);

    /* The next hit is the new sample: 500 frames, so gone after eight blocks. */
    rig.engine.noteOn(0, 127);
    REQUIRE(rig.run(1) > 0.0f);
    rig.run(7);
    REQUIRE_FALSE(rig.engine.padSounding(0));
    REQUIRE(rig.engine.counters().underruns == 0);
}

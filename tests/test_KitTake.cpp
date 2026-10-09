// SPDX-License-Identifier: MIT
//
// The native take's copy of a kit, the gate that starts its worker, and the
// arena-loan rule -- the parts of the board's take that need no board.
#include <catch2/catch_test_macros.hpp>

#include "crosspad/kit/KitPathUtils.hpp"
#include "crosspad/sampler/KitTake.hpp"

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace crosspad;

namespace {

std::unique_ptr<KitInfo> makeKit()
{
    auto kit = std::make_unique<KitInfo>("/crosspad/kits/BLACK TESLA/kit.json");
    kit->name = "BLACK TESLA";
    kit->parsed = true;
    for (uint8_t p = 0; p < KIT_PADS; ++p) {
        Pad& pad = kit->pads[p];
        pad.number = p;
        pad.playbackMode = static_cast<uint8_t>(p % 3);
        pad.choke_group = p % 4;
        pad.volume = 50 + p;
        pad.pan = -32 + p;
        pad.pitch = p - 8;
        pad.cycle = (p & 1) != 0;
        pad.AddSample("pad" + std::to_string(p) + ".wav", false);
        pad.samples.back().start = 10u * p;
        pad.samples.back().end = 1000u + p;
    }
    kit->pads[3].hp = 200;
    kit->pads[4].reverse = true;
    kit->pads[5].lp = 8000;
    kit->pads[6].AddSample("soft.wav", false);
    kit->pads[6].AddSample("hard.wav", false);
    return kit;
}

} // namespace

TEST_CASE("A take's copy holds everything the take reads, and outlives the kit", "[kittake]") {
    auto spec = std::make_unique<TakeSpec>();
    {
        auto kit = makeKit();
        REQUIRE(buildTakeSpec(*kit, 7, *spec));
        /* The kit goes, as releaseRetiredKit() takes a retired one -- the copy
         * must not point into it. Scribble first, so a pointer that survived
         * would read garbage rather than happen to still be right. */
        for (auto& pad : kit->pads) {
            for (auto& s : pad.samples) s.sample.assign(200, 'x');
        }
        kit->name.assign(64, 'y');
        kit->path.assign(300, 'z');
    }

    CHECK(spec->kitId == 7);
    CHECK(std::string(spec->name) == "BLACK TESLA");
    CHECK(std::string(spec->kitPath) == "/crosspad/kits/BLACK TESLA/kit.json");
    CHECK(spec->layerCount == 18);                 // 16 pads, pad 6 has three
    CHECK(spec->layersDropped == 0);
    for (uint8_t p = 0; p < KIT_PADS; ++p) {
        INFO("pad " << int(p));
        const TakePad& tp = spec->pads[p];
        CHECK(tp.mode == p % 3);
        CHECK(tp.choke == p % 4);
        CHECK(tp.volume == 50 + p);
        CHECK(tp.pan == -32 + p);
        CHECK(tp.pitch == p - 8);
        CHECK(tp.cycle == ((p & 1) != 0));
        REQUIRE(tp.layerCount == (p == 6 ? 3 : 1));
        const TakeLayer& l = spec->layers[tp.firstLayer];
        CHECK(std::string(l.sample) == "pad" + std::to_string(p) + ".wav");
        CHECK(l.start == 10u * p);
        CHECK(l.end == 1000u + p);
    }
    CHECK(std::string(spec->layers[spec->pads[6].firstLayer + 2].sample) == "hard.wav");
    CHECK(spec->pads[3].hp == 200);
    CHECK(spec->pads[4].reverse);
    CHECK(spec->pads[5].lp == 8000);
    CHECK(spec->pads[3].wantsCopy());
    CHECK_FALSE(spec->pads[0].wantsCopy());        // pitch alone is the engine's

    /* The paths the worker resolves come out as the board's loader names them. */
    const TakePad& p4 = spec->pads[4];
    const std::string logical = takeLayerLogical(spec->kitPath, spec->layers[p4.firstLayer]);
    CHECK(logical == "/crosspad/kits/BLACK TESLA/pad4.wav");
    CHECK(takeLayerCopy(p4, logical) == renderedSamplePath(logical, p4.fx()));
    CHECK(takeLayerCopy(spec->pads[0], logical).empty());
}

TEST_CASE("A take's copy refuses what no take plays and caps what a take holds", "[kittake]") {
    auto spec = std::make_unique<TakeSpec>();
    auto kit = makeKit();

    kit->parsed = false;
    CHECK_FALSE(buildTakeSpec(*kit, 1, *spec));
    kit->parsed = true;
    kit->engine = KitEngine::Pitched;
    CHECK_FALSE(buildTakeSpec(*kit, 1, *spec));
    kit->engine = KitEngine::Sampler;

    /* Eight layers on every pad is 128; a take holds 64 samples. */
    for (auto& pad : kit->pads) {
        while (pad.sampleCount < KitPad::kMaxLayers) pad.AddSample("more.wav", false);
    }
    /* And a name too long to keep whole is left out, not cut into another name. */
    kit->pads[0].samples[0].sample.assign(SampleStore::kMaxPath + 10, 'n');
    REQUIRE(buildTakeSpec(*kit, 1, *spec));
    CHECK(spec->layerCount == TakeSpec::kMaxLayers);
    CHECK(spec->layersDropped == 16 * KitPad::kMaxLayers - TakeSpec::kMaxLayers);
    CHECK(spec->pads[0].layerCount == KitPad::kMaxLayers - 1);
}

TEST_CASE("A pad's copy for a rebuild is the same pad, standalone", "[kittake]") {
    auto kit = makeKit();
    TakePadSpec ps;
    REQUIRE(buildTakePadSpec(*kit, 3, 6, ps));
    kit.reset();
    CHECK(ps.kitId == 3);
    CHECK(ps.pad.firstLayer == 0);
    REQUIRE(ps.pad.layerCount == 3);
    CHECK(std::string(ps.layers[1].sample) == "soft.wav");
    CHECK(ps.pad.volume == 56);
    CHECK_FALSE(buildTakePadSpec(*makeKit(), 3, KIT_PADS, ps));
}

TEST_CASE("A retake asked for during a take is served after it", "[kittake]") {
    WorkerGate gate;
    REQUIRE(gate.post());                 // nobody running: start the worker
    REQUIRE(gate.next());                 // the worker takes the first kit
    CHECK_FALSE(gate.post());             // another kit lands mid-take: no second worker
    CHECK_FALSE(gate.post());             // and a third: still one pass owed (newest wins)
    REQUIRE(gate.next());                 // the take ends; the retake is there
    CHECK_FALSE(gate.next());             // nothing more: the worker leaves
    CHECK_FALSE(gate.running());
    CHECK(gate.post());                   // the next request starts a new one
    gate.failed();                        // it could not be created
    CHECK_FALSE(gate.running());
    CHECK(gate.post());                   // and the one after tries again
}

TEST_CASE("No request is lost to a worker on its way out", "[kittake]") {
    /* Every post() is followed by a pass that starts after it -- by the worker
     * that was running, or by one the poster started. A request that lands in
     * the worker's last instant is where a flag-and-handle scheme loses it. */
    for (int round = 0; round < 200; ++round) {
        WorkerGate gate;
        std::atomic<uint32_t> asked{0}, served{0};
        std::vector<std::thread> workers;
        auto worker = [&] {
            while (gate.next()) {
                served.store(asked.load(std::memory_order_acquire), std::memory_order_release);
                std::this_thread::yield();
            }
        };
        for (uint32_t i = 1; i <= 300; ++i) {
            asked.store(i, std::memory_order_release);
            if (gate.post()) workers.emplace_back(worker);
            if ((i & 7) == 0) std::this_thread::yield();
        }
        for (auto& t : workers) t.join();
        REQUIRE(served.load() == 300u);
        REQUIRE_FALSE(gate.running());
    }
}

TEST_CASE("The arena lends only what its largest block can spare", "[kittake][arena]") {
    constexpr size_t kB = 1024;
    const size_t keep = arenaLendKeep(24 * 64 * kB, 16 * 64 * kB, 128 * kB, 2);
    CHECK(keep == 8 * 64 * kB + 256 * kB);

    /* One free block of 2 MB: a 64 kB ring fits beside what is kept. */
    CHECK(arenaMayLend(64 * kB, 2048 * kB, keep));
    /* The same 2 MB free in 48 kB pieces: the total says yes, but no page and
     * no head fits any more once a piece is lent -- refused. */
    CHECK_FALSE(arenaMayLend(64 * kB, 48 * kB, keep));
    CHECK_FALSE(arenaMayLend(32 * kB, 300 * kB, keep));
    /* Exactly enough is enough. */
    CHECK(arenaMayLend(64 * kB, 64 * kB + keep, keep));
    CHECK_FALSE(arenaMayLend(64 * kB + 1, 64 * kB + keep, keep));
    /* A pool already at its ceiling keeps only the heads. */
    CHECK(arenaLendKeep(24 * 64 * kB, 24 * 64 * kB, 256 * kB, 2) == 512 * kB);
    CHECK(arenaLendKeep(24 * 64 * kB, 30 * 64 * kB, 256 * kB, 2) == 512 * kB);
}

// ── The hand-over around the gate: newest wins, and a state that never sticks ─

TEST_CASE("A retake folded into the pass before it does not leave the state busy", "[kittake]") {
    /* Merge re-review N1: the worker's next() takes A's request; B's copy
     * replaces A's and B posts again; the worker takes B's copy (A's is gone),
     * finishes, and the extra pass B's post owes finds nothing. The state used
     * to be stored only when nothing was pending, so it stayed "taking" for
     * good while the engine held the right kit. */
    LatestWork<int> q;
    int a = 1, b = 2;
    int* displaced = nullptr;
    REQUIRE(q.offer(&a, displaced));               // start the worker
    CHECK(displaced == nullptr);
    CHECK(q.state() == LatestWork<int>::kBusy);
    REQUIRE(q.next());                             // A's request
    CHECK_FALSE(q.offer(&b, displaced));           // B lands before the worker takes the copy
    CHECK(displaced == &a);                        // B frees A's copy
    int* got = q.take();
    REQUIRE(got == &b);                            // the worker takes B's
    q.done(LatestWork<int>::kReady);
    CHECK(q.state() == LatestWork<int>::kBusy);    // B's pass is still owed
    REQUIRE(q.next());
    CHECK(q.take() == nullptr);                    // ...and finds nothing
    CHECK_FALSE(q.next());                         // the worker leaves
    CHECK(q.state() == LatestWork<int>::kReady);   // not stuck at busy
}

TEST_CASE("The state is busy from the request on, never a stale ready", "[kittake]") {
    LatestWork<int> q;
    int a = 1;
    int* displaced = nullptr;
    q.done(LatestWork<int>::kReady);               // a kit taken earlier
    CHECK(q.state() == LatestWork<int>::kReady);
    REQUIRE(q.offer(&a, displaced));
    CHECK(q.state() == LatestWork<int>::kBusy);    // at once, before any worker runs
    REQUIRE(q.next());
    CHECK(q.take() == &a);
    q.done(LatestWork<int>::kFailed);
    CHECK_FALSE(q.next());
    CHECK(q.state() == LatestWork<int>::kFailed);
    q.settle(LatestWork<int>::kIdle);              // released by someone else
    CHECK(q.state() == LatestWork<int>::kIdle);
}

TEST_CASE("A worker that could not be started gives the copy back and says failed", "[kittake]") {
    LatestWork<int> q;
    int a = 1;
    int* displaced = nullptr;
    REQUIRE(q.offer(&a, displaced));
    CHECK(q.startFailed() == &a);
    CHECK(q.state() == LatestWork<int>::kFailed);
    REQUIRE(q.offer(&a, displaced));               // the next request starts one again
}

TEST_CASE("After any burst of requests the last one is served and the state settles", "[kittake]") {
    for (int round = 0; round < 100; ++round) {
        LatestWork<uint32_t> q;
        std::vector<uint32_t> items(400);
        std::atomic<uint32_t> served{0};
        std::vector<std::thread> workers;
        auto worker = [&] {
            while (q.next()) {
                uint32_t* it = q.take();
                if (!it) continue;
                served.store(*it, std::memory_order_release);
                std::this_thread::yield();
                q.done(LatestWork<uint32_t>::kReady);
            }
        };
        for (uint32_t i = 0; i < items.size(); ++i) {
            items[i] = i + 1;
            uint32_t* displaced = nullptr;
            if (q.offer(&items[i], displaced)) workers.emplace_back(worker);
            if ((i & 7) == 0) std::this_thread::yield();
        }
        for (auto& t : workers) t.join();
        REQUIRE(served.load() == items.size());
        REQUIRE(q.state() == LatestWork<uint32_t>::kReady);
    }
}

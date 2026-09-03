// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include "crosspad/instrument/SampleBank.hpp"

#include <cstdlib>
#include <vector>

using namespace crosspad;

namespace {
int g_live = 0;
void* bankAlloc(size_t n) { ++g_live; return std::malloc(n); }
void  bankFree(void* p)   { --g_live; std::free(p); }
}

TEST_CASE("SampleBank: allocates, owns and frees its PCM", "[instrument][bank]") {
    g_live = 0;
    {
        SampleBank bank(bankAlloc, bankFree);
        REQUIRE(bank.zoneCount() == 0);
        REQUIRE(bank.bytes() == 0);
        REQUIRE(bank.nearestZone(60.0f) == nullptr);

        int16_t* p = bank.allocPcm(1000);
        REQUIRE(p != nullptr);
        REQUIRE(bank.bytes() == 2000);
        REQUIRE(bank.allocPcm(0) == nullptr);

        Zone z; z.pcm = p; z.length = 1000; z.rootMidi = 60.0f;
        REQUIRE(bank.addZone(z));
        REQUIRE(bank.zoneCount() == 1);
        REQUIRE(bank.zone(0).length == 1000);
        REQUIRE(bank.zone(9).pcm == nullptr);   // out of range -> empty zone

        // PCM the bank does not own is refused.
        std::vector<int16_t> foreign(10);
        Zone bad; bad.pcm = foreign.data(); bad.length = 10;
        REQUIRE_FALSE(bank.addZone(bad));
    }
    REQUIRE(g_live == 0);
}

TEST_CASE("SampleBank: caps at kMaxZones", "[instrument][bank]") {
    g_live = 0;
    {
        SampleBank bank(bankAlloc, bankFree);
        for (int i = 0; i < static_cast<int>(SampleBank::kMaxZones); ++i) {
            int16_t* p = bank.allocPcm(16);
            REQUIRE(p != nullptr);
            Zone z; z.pcm = p; z.length = 16; z.rootMidi = 60.0f + i;
            REQUIRE(bank.addZone(z));
        }
        REQUIRE(bank.zoneCount() == SampleBank::kMaxZones);
        REQUIRE(bank.allocPcm(16) == nullptr);
    }
    REQUIRE(g_live == 0);
}

TEST_CASE("SampleBank: nearestZone picks the nearest root, ties go low", "[instrument][bank]") {
    SampleBank bank(bankAlloc, bankFree);
    for (float root : {57.0f, 69.0f}) {
        int16_t* p = bank.allocPcm(8);
        REQUIRE(p != nullptr);
        Zone z; z.pcm = p; z.length = 8; z.rootMidi = root;
        REQUIRE(bank.addZone(z));
    }
    REQUIRE(bank.nearestZone(63.0f)->rootMidi == 57.0f);   // exact tie -> lower
    REQUIRE(bank.nearestZone(64.0f)->rootMidi == 69.0f);
    REQUIRE(bank.nearestZone(62.0f)->rootMidi == 57.0f);
    REQUIRE(bank.nearestZone(0.0f)->rootMidi == 57.0f);
    REQUIRE(bank.nearestZone(127.0f)->rootMidi == 69.0f);
}

TEST_CASE("SampleBank: a degenerate loop becomes a one-shot", "[instrument][bank]") {
    SampleBank bank(bankAlloc, bankFree);
    int16_t* p = bank.allocPcm(100);
    REQUIRE(p != nullptr);
    Zone z; z.pcm = p; z.length = 100; z.loopStart = 50; z.loopEnd = 40;
    REQUIRE(bank.addZone(z));
    REQUIRE(bank.zone(0).loopEnd == 0);
    REQUIRE(bank.zone(0).loopStart == 0);
}

TEST_CASE("SampleBank: rejects loops that are too short or run past the end",
          "[instrument][bank]") {
    SampleBank bank(bankAlloc, bankFree);
    int16_t* p = bank.allocPcm(100);
    REQUIRE(p != nullptr);

    // Under kMinLoopSamples: the renderer would wrap several times per output
    // frame at any transposition, which is what made the old subtract-loop
    // wrap unbounded work on the RT task.
    Zone tooShort; tooShort.pcm = p; tooShort.length = 100;
    tooShort.loopStart = 10; tooShort.loopEnd = 13;      // span 3
    REQUIRE_FALSE(bank.addZone(tooShort));

    Zone pastEnd; pastEnd.pcm = p; pastEnd.length = 100;
    pastEnd.loopStart = 90; pastEnd.loopEnd = 120;
    REQUIRE_FALSE(bank.addZone(pastEnd));

    REQUIRE(bank.zoneCount() == 0);

    Zone ok; ok.pcm = p; ok.length = 100;
    ok.loopStart = 10; ok.loopEnd = 10 + SampleBank::kMinLoopSamples;
    REQUIRE(bank.addZone(ok));
    REQUIRE(bank.zone(0).loopStart == 10);
    REQUIRE(bank.zone(0).loopEnd == 10 + SampleBank::kMinLoopSamples);
}

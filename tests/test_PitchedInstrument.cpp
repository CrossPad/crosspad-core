// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "crosspad/instrument/PitchedInstrument.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

using namespace crosspad;

namespace {

void* bankAlloc(size_t n) { return std::malloc(n); }
void  bankFree(void* p)   { std::free(p); }

constexpr uint32_t kFrames = 64;
constexpr uint32_t kSr = 44100;
const float kCenterGain = std::sqrt(0.5f);   // equal-power pan, centre

/// A bank with one zone whose sample k holds `fill(k)`.
SampleBank* makeBank(uint32_t length, float root, uint32_t loopStart, uint32_t loopEnd,
                     int16_t (*fill)(uint32_t)) {
    auto* bank = new SampleBank(bankAlloc, bankFree);
    int16_t* p = bank->allocPcm(length);
    for (uint32_t i = 0; i < length; ++i) p[i] = fill(i);
    Zone z;
    z.pcm = p; z.length = length; z.rootMidi = root;
    z.loopStart = loopStart; z.loopEnd = loopEnd;
    bank->addZone(z);
    return bank;
}

int16_t rampFill(uint32_t i) { return static_cast<int16_t>(i); }
int16_t constFill(uint32_t)  { return 1000; }

InstrumentParams params(uint8_t maxVoices, float attackMs = 0.0f, float releaseMs = 150.0f) {
    InstrumentParams p;
    p.maxVoices = maxVoices;
    p.attackMs = attackMs;
    p.releaseMs = releaseMs;
    return p;
}

} // namespace

TEST_CASE("PitchedInstrument: is a generator named Pitched", "[instrument][engine]") {
    PitchedInstrument inst;
    REQUIRE(inst.isGenerator());
    REQUIRE(std::string(inst.name()) == "Pitched");
    REQUIRE(inst.activeVoices() == 0);
}

TEST_CASE("PitchedInstrument: no bank renders nothing", "[instrument][engine]") {
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.noteOn(69, 127, 0);
    REQUIRE(inst.activeVoices() == 0);

    std::vector<float> out(kFrames * 2, 0.25f);
    inst.mix(out.data(), kFrames);
    for (float v : out) REQUIRE(v == 0.25f);
}

TEST_CASE("PitchedInstrument: an octave up reads two samples per frame", "[instrument][engine]") {
    SampleBank* bank = makeBank(10000, 57.0f, 0, 0, rampFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(24));
    inst.setBank(bank);
    inst.noteOn(69, 127, 0);           // one octave above the root

    std::vector<float> out(kFrames * 2, 0.0f);
    inst.mix(out.data(), kFrames);

    for (uint32_t k = 0; k < kFrames; ++k) {
        const float want = static_cast<float>(2 * k) / 32768.0f * kCenterGain;
        REQUIRE(out[2 * k] == Catch::Approx(want).margin(1e-6));
    }
    // 64 frames advanced 128 source samples.
    delete bank;
}

TEST_CASE("PitchedInstrument: the root note plays 1:1", "[instrument][engine]") {
    SampleBank* bank = makeBank(10000, 57.0f, 0, 0, rampFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(24));
    inst.setBank(bank);
    inst.noteOn(57, 127, 0);

    std::vector<float> out(kFrames * 2, 0.0f);
    inst.mix(out.data(), kFrames);
    for (uint32_t k = 0; k < kFrames; ++k) {
        const float want = static_cast<float>(k) / 32768.0f * kCenterGain;
        REQUIRE(out[2 * k] == Catch::Approx(want).margin(1e-6));
    }
    delete bank;
}

TEST_CASE("PitchedInstrument: 16 notes fit 24 voices without stealing", "[instrument][engine]") {
    SampleBank* bank = makeBank(10000, 60.0f, 0, 0, constFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(24));
    inst.setBank(bank);

    for (uint8_t n = 0; n < 16; ++n) inst.noteOn(static_cast<uint8_t>(60 + n), 100, n);
    REQUIRE(inst.activeVoices() == 16);
    REQUIRE(inst.stealCount() == 0u);

    std::vector<float> out(kFrames * 2, 0.0f);
    inst.mix(out.data(), kFrames);
    REQUIRE(inst.activeVoices() == 16);
    REQUIRE(inst.stealCount() == 0u);
    delete bank;
}

TEST_CASE("PitchedInstrument: the 33rd note steals the oldest voice", "[instrument][engine]") {
    SampleBank* bank = makeBank(10000, 60.0f, 0, 0, constFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(32, 0.0f, 0.0f));
    inst.setBank(bank);

    for (int n = 0; n < 32; ++n) inst.noteOn(static_cast<uint8_t>(40 + n), 100, 0);
    REQUIRE(inst.activeVoices() == 32);
    REQUIRE(inst.stealCount() == 0u);

    inst.noteOn(100, 100, 0);          // 33rd, a note none of the others use
    REQUIRE(inst.activeVoices() == 32);
    REQUIRE(inst.stealCount() == 1u);

    // releaseMs 0 makes noteOff observable within one block: the oldest note
    // (40) no longer exists, so releasing it changes nothing, while the
    // second-oldest (41) still frees a voice.
    std::vector<float> out(kFrames * 2, 0.0f);
    inst.noteOff(40);
    inst.mix(out.data(), kFrames);
    REQUIRE(inst.activeVoices() == 32);
    inst.noteOff(41);
    inst.mix(out.data(), kFrames);
    REQUIRE(inst.activeVoices() == 31);
    delete bank;
}

TEST_CASE("PitchedInstrument: retrigger releases the old voice", "[instrument][engine]") {
    SampleBank* bank = makeBank(10000, 60.0f, 0, 0, constFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(24));
    inst.setBank(bank);

    inst.noteOn(60, 100, 0);
    REQUIRE(inst.activeVoices() == 1);
    inst.noteOn(60, 100, 0);
    REQUIRE(inst.activeVoices() == 2);   // old voice released, not cut
    REQUIRE(inst.stealCount() == 0u);
    delete bank;
}

TEST_CASE("PitchedInstrument: release tail lasts releaseMs", "[instrument][engine]") {
    SampleBank* bank = makeBank(200000, 60.0f, 0, 0, constFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(24, 0.0f, 150.0f));
    inst.setBank(bank);

    std::vector<float> out(kFrames * 2, 0.0f);
    inst.noteOn(60, 127, 0);
    inst.mix(out.data(), kFrames);
    REQUIRE(inst.activeVoices() == 1);

    inst.noteOff(60);
    const int blocks = static_cast<int>(std::ceil(0.15 * kSr / kFrames)) + 1;
    for (int b = 0; b < blocks - 2; ++b) inst.mix(out.data(), kFrames);
    REQUIRE(inst.activeVoices() == 1);          // still ringing just before the end
    inst.mix(out.data(), kFrames);
    inst.mix(out.data(), kFrames);
    REQUIRE(inst.activeVoices() == 0);
    delete bank;
}

TEST_CASE("PitchedInstrument: a looped zone plays past its length", "[instrument][engine]") {
    SampleBank* bank = makeBank(100, 60.0f, 0, 100, constFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(24, 0.0f, 5000.0f));
    inst.setBank(bank);
    inst.noteOn(60, 127, 0);

    std::vector<float> out(kFrames * 2, 0.0f);
    for (int b = 0; b < 40; ++b) {          // 2560 frames >> 100 samples
        std::fill(out.begin(), out.end(), 0.0f);
        inst.mix(out.data(), kFrames);
    }
    REQUIRE(inst.activeVoices() == 1);
    // Still producing the constant zone value.
    const float want = 1000.0f / 32768.0f * kCenterGain;
    REQUIRE(out[0] == Catch::Approx(want).margin(1e-4));
    delete bank;
}

TEST_CASE("PitchedInstrument: mix adds into the buffer", "[instrument][engine]") {
    SampleBank* bank = makeBank(10000, 60.0f, 0, 0, constFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(24));
    inst.setBank(bank);
    inst.noteOn(60, 127, 0);

    std::vector<float> out(kFrames * 2, 1.0f);
    inst.mix(out.data(), kFrames);
    const float want = 1.0f + 1000.0f / 32768.0f * kCenterGain;
    REQUIRE(out[0] == Catch::Approx(want).margin(1e-6));
    REQUIRE(out[1] == Catch::Approx(want).margin(1e-6));
    delete bank;
}

TEST_CASE("PitchedInstrument: hard left pan leaves the right channel silent", "[instrument][engine]") {
    SampleBank* bank = makeBank(10000, 60.0f, 0, 0, constFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(24));
    inst.setBank(bank);
    inst.setPadMix(3, 100, -100);
    inst.noteOn(60, 127, 3);

    std::vector<float> out(kFrames * 2, 0.0f);
    inst.mix(out.data(), kFrames);
    for (uint32_t k = 0; k < kFrames; ++k) {
        REQUIRE(out[2 * k] == Catch::Approx(1000.0f / 32768.0f).margin(1e-6));
        REQUIRE(out[2 * k + 1] == 0.0f);
    }
    delete bank;
}

TEST_CASE("PitchedInstrument: pad volume scales the voice", "[instrument][engine]") {
    SampleBank* bank = makeBank(10000, 60.0f, 0, 0, constFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(24));
    inst.setBank(bank);
    inst.setPadMix(5, 50, 0);
    inst.noteOn(60, 127, 5);

    std::vector<float> out(kFrames * 2, 0.0f);
    inst.mix(out.data(), kFrames);
    const float want = 1000.0f / 32768.0f * kCenterGain * 0.5f;
    REQUIRE(out[0] == Catch::Approx(want).margin(1e-6));
    delete bank;
}

TEST_CASE("PitchedInstrument: never renders past onPrepare's maxFrames", "[instrument][engine]") {
    SampleBank* bank = makeBank(10000, 60.0f, 0, 0, constFill);
    PitchedInstrument inst;
    inst.onPrepare(32, kSr);
    inst.setParams(params(24));
    inst.setBank(bank);
    inst.noteOn(60, 127, 0);

    std::vector<float> out(kFrames * 2, 0.0f);
    inst.mix(out.data(), kFrames);      // asks for 64, prepared for 32
    REQUIRE(out[2 * 31] != 0.0f);
    REQUIRE(out[2 * 32] == 0.0f);
    delete bank;
}

TEST_CASE("PitchedInstrument: allNotesOff and setBank clear voices", "[instrument][engine]") {
    SampleBank* bank = makeBank(10000, 60.0f, 0, 0, constFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(24));
    inst.setBank(bank);
    inst.noteOn(60, 127, 0);
    inst.noteOn(64, 127, 0);
    REQUIRE(inst.activeVoices() == 2);
    inst.allNotesOff();
    REQUIRE(inst.activeVoices() == 0);

    inst.noteOn(60, 127, 0);
    REQUIRE(inst.activeVoices() == 1);
    inst.setBank(nullptr);
    REQUIRE(inst.activeVoices() == 0);
    delete bank;
}

TEST_CASE("PitchedInstrument: noteOn during a mutation is ignored", "[instrument][engine]") {
    SampleBank* bank = makeBank(10000, 60.0f, 0, 0, constFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(24));
    inst.setBank(bank);

    inst.beginMutation();
    inst.noteOn(60, 127, 0);
    REQUIRE(inst.activeVoices() == 0);

    std::vector<float> out(kFrames * 2, 0.0f);
    inst.mix(out.data(), kFrames);
    for (float v : out) REQUIRE(v == 0.0f);

    inst.endMutation();
    inst.noteOn(60, 127, 0);
    REQUIRE(inst.activeVoices() == 1);
    delete bank;
}

TEST_CASE("PitchedInstrument: a voice cleared inside a mutation never renders again",
          "[instrument][engine]") {
    // The kit-load sequence the platform runs: the RT task is gated out, the
    // worker closes the note path, drops every voice, swaps the bank, and only
    // then frees the old one. A voice started just before beginMutation() must
    // not come back to life afterwards -- it points into the freed bank.
    SampleBank* oldBank = makeBank(10000, 60.0f, 0, 0, constFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(24, 0.0f, 5000.0f));
    inst.setBank(oldBank);

    inst.noteOn(60, 127, 0);
    REQUIRE(inst.activeVoices() == 1);

    inst.beginMutation();
    inst.noteOn(64, 127, 0);            // rejected: the path is closed
    inst.allNotesOff();
    SampleBank* newBank = makeBank(10000, 72.0f, 0, 0, rampFill);
    inst.setBank(newBank);
    inst.setParams(params(24, 0.0f, 5000.0f));
    inst.endMutation();

    delete oldBank;                      // safe only if no voice still holds it

    REQUIRE(inst.activeVoices() == 0);
    std::vector<float> out(kFrames * 2, 0.0f);
    for (int b = 0; b < 4; ++b) inst.mix(out.data(), kFrames);
    REQUIRE(inst.activeVoices() == 0);
    for (float v : out) REQUIRE(v == 0.0f);

    // The engine is alive again on the new bank.
    inst.noteOn(72, 127, 0);
    REQUIRE(inst.activeVoices() == 1);
    delete newBank;
}

TEST_CASE("PitchedInstrument: a 4-sample loop at ratio 16 wraps by modulo",
          "[instrument][engine]") {
    // Shortest loop SampleBank accepts, played four octaves up: the phase
    // clears the loop span four times per output frame. The old wrap subtracted
    // one span at a time; what matters here is that the block still produces
    // exactly `frames` samples and the read position never leaves the loop.
    constexpr uint32_t kLoopStart = 8;
    constexpr uint32_t kLoopEnd = 12;      // span == kMinLoopSamples
    SampleBank* bank = makeBank(64, 60.0f, kLoopStart, kLoopEnd, rampFill);
    REQUIRE(bank->zoneCount() == 1);

    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(24, 0.0f, 5000.0f));
    inst.setBank(bank);
    inst.noteOn(108, 127, 0);              // 48 semitones up -> ratio 16

    std::vector<float> out(kFrames * 2, 0.0f);
    inst.mix(out.data(), kFrames);
    REQUIRE(inst.activeVoices() == 1);

    // Every frame was written (the interpolated ramp inside [8, 12) is well
    // above zero) and none of them reached the end of the 64-sample zone.
    const float lowest  = static_cast<float>(kLoopStart - 1) / 32768.0f * kCenterGain;
    const float highest = static_cast<float>(kLoopEnd + 1) / 32768.0f * kCenterGain;
    for (uint32_t k = 0; k < kFrames; ++k) {
        REQUIRE(out[2 * k] > lowest);
        REQUIRE(out[2 * k] < highest);
    }
    delete bank;
}

TEST_CASE("PitchedInstrument: lowering maxVoices releases the voices above the cap",
          "[instrument][engine]") {
    SampleBank* bank = makeBank(200000, 60.0f, 0, 0, constFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(8, 0.0f, 0.0f));
    inst.setBank(bank);

    for (uint8_t n = 0; n < 8; ++n) inst.noteOn(static_cast<uint8_t>(60 + n), 100, 0);
    REQUIRE(inst.activeVoices() == 8);

    // Slots 2..7 are now above the cap: nothing would ever reallocate or steal
    // them, so setParams() has to put them into release itself.
    inst.setParams(params(2, 0.0f, 0.0f));
    std::vector<float> out(kFrames * 2, 0.0f);
    inst.mix(out.data(), kFrames);
    REQUIRE(inst.activeVoices() == 2);

    // And the two that survive are still the ones the allocator can reach.
    inst.noteOn(90, 100, 0);
    REQUIRE(inst.activeVoices() == 2);
    REQUIRE(inst.stealCount() == 1u);
    delete bank;
}

TEST_CASE("PitchedInstrument: gateEnter succeeds when no render is in flight",
          "[instrument][gate]") {
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    SampleBank* bank = makeBank(4096, 69.0f, 0, 0, constFill);
    bool safe = false;
    delete inst.installBank(bank, params(8), nullptr, 10, nullptr, &safe);
    REQUIRE(safe);

    // Nothing is rendering, so the very first look answers — including with
    // maxSpins == 0, which must not mean "never check".
    REQUIRE(inst.gateEnter(10, nullptr));
    inst.gateExit();
    REQUIRE(inst.gateEnter(0, nullptr));
    inst.gateExit();

    delete inst.installBank(nullptr, params(8), nullptr, 10, nullptr, &safe);
}

TEST_CASE("PitchedInstrument: a render while the gate is held contributes nothing",
          "[instrument][gate]") {
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    SampleBank* bank = makeBank(4096, 69.0f, 0, 0, constFill);
    bool safe = false;
    delete inst.installBank(bank, params(8), nullptr, 10, nullptr, &safe);

    inst.noteOn(69, 127, 0);
    REQUIRE(inst.activeVoices() == 1);

    // Sounding, so an ungated block is audible.
    std::vector<float> out(kFrames * 2, 0.0f);
    inst.mix(out.data(), kFrames);
    REQUIRE(std::fabs(out[0]) > 0.0f);

    // Under the gate the same voice writes nothing at all — mix() returns
    // before it touches the engine, which is what lets the loader free the
    // bank the voice is pointing at.
    REQUIRE(inst.gateEnter(10, nullptr));
    std::fill(out.begin(), out.end(), 0.0f);
    inst.mix(out.data(), kFrames);
    for (float v : out) REQUIRE(v == 0.0f);

    // A note that arrives inside the swap window is dropped, not queued: the
    // bank it would point into is the one about to go.
    inst.beginMutation();
    inst.allNotesOff();
    inst.noteOn(72, 127, 0);
    REQUIRE(inst.activeVoices() == 0);
    inst.endMutation();
    inst.gateExit();

    delete inst.installBank(nullptr, params(8), nullptr, 10, nullptr, &safe);
}

TEST_CASE("PitchedInstrument: installBank hands back the previous bank",
          "[instrument][gate]") {
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);

    SampleBank* first = makeBank(4096, 69.0f, 0, 0, constFill);
    bool safe = false;
    REQUIRE(inst.installBank(first, params(8), nullptr, 10, nullptr, &safe) == nullptr);
    REQUIRE(safe);
    REQUIRE(inst.bank() == first);

    inst.noteOn(69, 127, 0);
    REQUIRE(inst.activeVoices() == 1);

    // The swap silences whatever was sounding out of the old bank before it
    // publishes the new one, so `first` is genuinely unreferenced on return.
    SampleBank* second = makeBank(2048, 60.0f, 0, 0, rampFill);
    safe = false;
    SampleBank* old = inst.installBank(second, params(4), nullptr, 10, nullptr, &safe);
    REQUIRE(old == first);
    REQUIRE(safe);
    REQUIRE(inst.activeVoices() == 0);
    REQUIRE(inst.bank() == second);
    delete old;

    // Pad mix comes from the kit when one is given.
    KitInfo kit;
    kit.pads[3].volume = 50;
    kit.pads[3].pan = -100;
    SampleBank* third = makeBank(2048, 60.0f, 0, 0, constFill);
    delete inst.installBank(third, params(4), &kit, 10, nullptr, &safe);
    inst.noteOn(60, 127, 3);
    std::vector<float> out(kFrames * 2, 0.0f);
    inst.mix(out.data(), kFrames);
    // Hard left at half volume: the right channel stays silent.
    REQUIRE(std::fabs(out[0]) > 0.0f);
    REQUIRE(std::fabs(out[1]) < 1e-6f);

    delete inst.installBank(nullptr, params(4), nullptr, 10, nullptr, &safe);
}

TEST_CASE("PitchedInstrument: gateEnter yields while a render is in flight",
          "[instrument][gate]") {
    // A renderer that never leaves must be reported as never left, with the
    // gate still held — that answer is what decides whether a bank may be
    // freed, so it has to be the pessimistic one.
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    SampleBank* bank = makeBank(4096, 69.0f, 0, 0, constFill);
    bool safe = false;
    delete inst.installBank(bank, params(8), nullptr, 10, nullptr, &safe);

    static int yields = 0;
    yields = 0;
    // The gate is only observed from inside mix(); with no audio thread here
    // the honest check is that the yield is actually called the promised
    // number of times before the answer comes back.
    REQUIRE(inst.gateEnter(5, [] { ++yields; }));
    REQUIRE(yields == 0);              // nothing rendering: answered on the first look
    inst.gateExit();

    delete inst.installBank(nullptr, params(8), nullptr, 10, nullptr, &safe);
}

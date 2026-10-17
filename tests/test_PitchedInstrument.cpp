// SPDX-License-Identifier: GPL-3.0-or-later
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

TEST_CASE("PitchedInstrument: maxVoices keeps the fade slots back", "[instrument][engine]") {
    SampleBank* bank = makeBank(10000, 60.0f, 0, 0, constFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(PitchedInstrument::kMaxVoices, 0.0f, 0.0f));   // ask for every slot
    inst.setBank(bank);

    // The cap is the slot count minus the fade reserve, whatever was asked for.
    for (int n = 0; n < PitchedInstrument::kMaxSounding; ++n) {
        inst.noteOn(static_cast<uint8_t>(40 + n), 100, 0);
    }
    REQUIRE(inst.soundingVoices() == PitchedInstrument::kMaxSounding);
    REQUIRE(inst.activeVoices() == PitchedInstrument::kMaxSounding);
    REQUIRE(inst.stealCount() == 0u);

    inst.noteOn(100, 100, 0);          // one past the cap
    REQUIRE(inst.stealCount() == 1u);
    // The new note sounds; the stolen one is still in the array, fading in a
    // slot of its own.
    REQUIRE(inst.soundingVoices() == PitchedInstrument::kMaxSounding);
    REQUIRE(inst.activeVoices() == PitchedInstrument::kMaxSounding + 1);
    delete bank;
}

TEST_CASE("PitchedInstrument: a stolen voice fades instead of cutting",
          "[instrument][engine]") {
    SampleBank* bank = makeBank(200000, 60.0f, 0, 0, constFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(1, 0.0f, 150.0f));    // one sounding voice: note 2 steals note 1
    inst.setBank(bank);

    inst.noteOn(60, 100, 0);
    std::vector<float> alone(kFrames * 2, 0.0f);
    inst.mix(alone.data(), kFrames);

    inst.noteOn(72, 100, 0);
    REQUIRE(inst.stealCount() == 1u);
    REQUIRE(inst.soundingVoices() == 1);
    REQUIRE(inst.activeVoices() == 2);

    // 5 ms is 220.5 frames at 44.1 kHz: the fade is still running after one
    // 64-frame block and its level is falling, which a hard cut would not do.
    std::vector<float> out(kFrames * 2, 0.0f);
    inst.mix(out.data(), kFrames);
    REQUIRE(inst.activeVoices() == 2);
    REQUIRE(out[0] > out[2 * (kFrames - 1)]);
    REQUIRE(out[2 * (kFrames - 1)] > alone[0]);      // both voices are still in it

    // Still there after three blocks (192 frames)...
    inst.mix(out.data(), kFrames);
    inst.mix(out.data(), kFrames);
    REQUIRE(inst.activeVoices() == 2);
    // ...and gone by the end of the fourth (256).
    inst.mix(out.data(), kFrames);
    REQUIRE(inst.activeVoices() == 1);
    REQUIRE(inst.soundingVoices() == 1);
    delete bank;
}

TEST_CASE("PitchedInstrument: a release tail is stolen before a held note",
          "[instrument][engine]") {
    SampleBank* bank = makeBank(200000, 60.0f, 0, 0, constFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(3, 0.0f, 1000.0f));
    inst.setBank(bank);

    // 60 is the oldest and still held; 62, the newest, is in a long release.
    inst.noteOn(60, 100, 0);
    inst.noteOn(61, 100, 0);
    inst.noteOn(62, 100, 0);
    inst.noteOff(62);
    inst.noteOn(63, 100, 0);           // over the cap: someone gives way
    REQUIRE(inst.stealCount() == 1u);
    REQUIRE(inst.soundingVoices() == 3);

    // Releases from here on are instant, so a held 60 and 61 end at once.
    // Had 60 been stolen, 62's 1 s tail would still be sounding.
    inst.setParams(params(3, 0.0f, 0.0f));
    inst.noteOff(60);
    inst.noteOff(61);
    std::vector<float> out(kFrames * 2, 0.0f);
    for (int b = 0; b < 4; ++b) inst.mix(out.data(), kFrames);   // the 5 ms fade is over
    REQUIRE(inst.activeVoices() == 1);                            // only 63
    delete bank;
}

TEST_CASE("PitchedInstrument: the voice cap clamps every setParams", "[instrument][engine]") {
    PitchedInstrument inst;
    REQUIRE(inst.voiceCap() == PitchedInstrument::kMaxSounding);

    // A kit or the editor asking for 24 under the board's cap of 10 gets 10,
    // every time.
    inst.setVoiceCap(10);
    inst.setParams(params(24));
    REQUIRE(inst.maxVoices() == 10);
    inst.setParams(params(8));
    REQUIRE(inst.maxVoices() == 8);
    inst.setParams(params(PitchedInstrument::kMaxSounding));
    REQUIRE(inst.maxVoices() == 10);

    // The cap itself stays within 1..kMaxSounding.
    inst.setVoiceCap(0);
    REQUIRE(inst.voiceCap() == 1);
    inst.setVoiceCap(200);
    REQUIRE(inst.voiceCap() == PitchedInstrument::kMaxSounding);
}

TEST_CASE("PitchedInstrument: over budget a note takes a voice's place",
          "[instrument][engine]") {
    SampleBank* bank = makeBank(200000, 60.0f, 0, 0, constFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(8, 0.0f, 150.0f));
    inst.setBank(bank);

    // Alone, a note always sounds.
    inst.setOverloaded(true);
    inst.noteOn(60, 100, 0);
    REQUIRE(inst.soundingVoices() == 1);
    REQUIRE(inst.stealCount() == 0u);

    inst.noteOn(61, 100, 0);           // two would be one more than the pass has room for
    REQUIRE(inst.soundingVoices() == 1);
    REQUIRE(inst.stealCount() == 1u);
    REQUIRE(inst.shedCount() == 1u);

    inst.setOverloaded(false);
    inst.noteOn(62, 100, 0);
    REQUIRE(inst.soundingVoices() == 2);
    REQUIRE(inst.stealCount() == 1u);
    inst.resetStats();
    REQUIRE(inst.shedCount() == 0u);
    delete bank;
}

TEST_CASE("PitchedInstrument: a fade is never stolen, only cut when nothing is left",
          "[instrument][engine]") {
    SampleBank* bank = makeBank(200000, 60.0f, 0, 0, constFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(1, 0.0f, 150.0f));
    inst.setBank(bank);

    // No mix() between notes, so no fade ever finishes: each note retires the
    // one sounding voice and takes a fresh slot until the array is full.
    for (int n = 0; n < PitchedInstrument::kMaxVoices; ++n) {
        inst.noteOn(static_cast<uint8_t>(40 + n), 100, 0);
    }
    REQUIRE(inst.activeVoices() == PitchedInstrument::kMaxVoices);
    REQUIRE(inst.soundingVoices() == 1);
    REQUIRE(inst.stealCount() == PitchedInstrument::kMaxVoices - 1);

    // Nothing free and nothing but fades to take: the oldest fade is cut, and
    // that is still counted as a steal.
    inst.noteOn(100, 100, 0);
    REQUIRE(inst.activeVoices() == PitchedInstrument::kMaxVoices);
    REQUIRE(inst.soundingVoices() == 1);
    REQUIRE(inst.stealCount() == PitchedInstrument::kMaxVoices);
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
    // Begin inside the loop: what is asserted is the wrap, not the attack.
    REQUIRE(bank->editZone(0, 60.0f, kLoopStart, kLoopEnd, SampleBank::kKeepXfade, kLoopStart));

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

    // And the next note is over the cap: the oldest of the two gives way, and
    // it is still audible while it fades.
    inst.noteOn(90, 100, 0);
    REQUIRE(inst.soundingVoices() == 2);
    REQUIRE(inst.activeVoices() == 3);
    REQUIRE(inst.stealCount() == 1u);
    delete bank;
}

TEST_CASE("PitchedInstrument: editZone moves the loop the next note plays",
          "[instrument][engine]") {
    SampleBank* bank = makeBank(10000, 60.0f, 0, 0, rampFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(4));
    inst.setBank(bank);

    REQUIRE(inst.editZone(0, 60.0f, 100, 200, 0, nullptr));
    REQUIRE(bank->zone(0).loopStart == 100);
    REQUIRE(bank->zone(0).loopEnd == 200);
    REQUIRE(bank->zone(0).start == 0);           // the start is its own point

    // At its root the zone plays 1:1, so the frame index is the sample index:
    // it starts at the zone's start, runs into the loop, and wraps to the loop
    // start -- not to where it began -- 200 samples in.
    inst.noteOn(60, 127, 0);
    std::vector<float> a(kFrames * 2, 0.0f), b(kFrames * 2, 0.0f),
                       c(kFrames * 2, 0.0f), d(kFrames * 2, 0.0f);
    inst.mix(a.data(), kFrames);
    inst.mix(b.data(), kFrames);
    inst.mix(c.data(), kFrames);
    inst.mix(d.data(), kFrames);

    const float scale = 1.0f / 32768.0f * kCenterGain;
    REQUIRE(a[0] == Catch::Approx(0.0f).margin(1e-6));
    REQUIRE(b[2 * 36] == Catch::Approx(100.0f * scale).margin(1e-6));
    REQUIRE(d[2 * 7] == Catch::Approx(199.0f * scale).margin(1e-6));
    REQUIRE(d[2 * 8] == Catch::Approx(100.0f * scale).margin(1e-6));

    // A start inside the loop: the next note begins there.
    REQUIRE(inst.editZone(0, 60.0f, 100, 200, 0, nullptr, SampleBank::kKeepXfade, 150));
    REQUIRE(bank->zone(0).start == 150);
    inst.noteOn(60, 127, 0);
    std::vector<float> e(kFrames * 2, 0.0f);
    inst.mix(e.data(), kFrames);
    REQUIRE(e[0] == Catch::Approx(150.0f * scale).margin(1e-6));

    // A root edit lands too, and clears the loop when asked to.
    REQUIRE(inst.editZone(0, 72.0f, 0, 0, 0, nullptr));
    REQUIRE(bank->zone(0).rootMidi == Catch::Approx(72.0f));
    REQUIRE(bank->zone(0).loopEnd == 0);
    delete bank;
}

TEST_CASE("PitchedInstrument: editZone changes nothing it cannot accept",
          "[instrument][engine]") {
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);

    // No bank at all.
    REQUIRE_FALSE(inst.editZone(0, 60.0f, 0, 0, 0, nullptr));

    SampleBank* bank = makeBank(10000, 60.0f, 100, 200, rampFill);
    inst.setBank(bank);

    REQUIRE_FALSE(inst.editZone(1, 60.0f, 0, 0, 0, nullptr));          // no such zone
    REQUIRE_FALSE(inst.editZone(0, 60.0f, 9000, 20000, 0, nullptr));   // past the end
    REQUIRE_FALSE(inst.editZone(0, 60.0f, 300, 302, 0, nullptr));      // shorter than the taps
    REQUIRE_FALSE(inst.editZone(0, 200.0f, 0, 0, 0, nullptr));         // not a MIDI note
    REQUIRE_FALSE(inst.editZone(0, -1.0f, 0, 0, 0, nullptr));

    REQUIRE(bank->zone(0).rootMidi == Catch::Approx(60.0f));
    REQUIRE(bank->zone(0).loopStart == 100);
    REQUIRE(bank->zone(0).loopEnd == 200);
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

TEST_CASE("PitchedInstrument: the scope tap carries the rendered mono sum",
          "[instrument][scope]") {
    // A ramp zone at its own root plays 1:1, so the block content is known
    // sample by sample and the tap can be checked against the audio, not
    // against itself.
    SampleBank* bank = makeBank(10000, 60.0f, 0, 0, rampFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(24));
    inst.setBank(bank);
    REQUIRE(inst.scopeTap().sampleRate() == kSr);

    inst.noteOn(60, 127, 0);
    const uint32_t blocks = 4;
    std::vector<float> audio(blocks * kFrames * 2, 0.0f);
    for (uint32_t b = 0; b < blocks; ++b) {
        inst.mix(audio.data() + b * kFrames * 2, kFrames);
    }

    std::vector<float> tapped(blocks * kFrames, 0.0f);
    REQUIRE(inst.scopeTap().snapshot(tapped.data(), blocks * kFrames) == blocks * kFrames);

    // One voice, centred: L and R are the same mono signal times cos/sin of a
    // 45-degree pan, so L + R is the mono sum scaled by 2 * cos(45).
    for (uint32_t k = 0; k < blocks * kFrames; ++k) {
        const float lr = audio[2 * k] + audio[2 * k + 1];
        REQUIRE(lr == Catch::Approx(tapped[k] * 2.0f * kCenterGain).margin(1e-5));
    }
    // And it really is the waveform: the ramp at 1:1 is sample k of the zone.
    for (uint32_t k = 0; k < blocks * kFrames; ++k) {
        REQUIRE(tapped[k] == Catch::Approx(static_cast<float>(k) / 32768.0f).margin(1e-6));
    }
    delete bank;
}

TEST_CASE("PitchedInstrument: silent blocks are written too", "[instrument][scope]") {
    // A view that stops being fed freezes on the last loud waveform. Every
    // block has to arrive, including the ones between notes.
    SampleBank* bank = makeBank(4096, 60.0f, 0, 0, constFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(24, 0.0f, 0.0f));
    inst.setBank(bank);

    std::vector<float> out(kFrames * 2, 0.0f);
    inst.mix(out.data(), kFrames);                 // no note has been played yet
    REQUIRE(inst.scopeTap().written() == kFrames);

    std::vector<float> tapped(kFrames, 1.0f);
    REQUIRE(inst.scopeTap().snapshot(tapped.data(), kFrames) == kFrames);
    for (float v : tapped) REQUIRE(v == 0.0f);

    inst.noteOn(60, 127, 0);
    inst.mix(out.data(), kFrames);
    REQUIRE(inst.scopeTap().snapshot(tapped.data(), kFrames) == kFrames);
    REQUIRE(std::fabs(tapped[0]) > 0.0f);

    inst.noteOff(60);                              // releaseMs 0: gone next block
    inst.mix(out.data(), kFrames);
    inst.mix(out.data(), kFrames);
    REQUIRE(inst.activeVoices() == 0);
    REQUIRE(inst.scopeTap().written() == 4u * kFrames);
    REQUIRE(inst.scopeTap().snapshot(tapped.data(), kFrames) == kFrames);
    for (float v : tapped) REQUIRE(v == 0.0f);     // decayed to a flat line

    // A block rendered while the loader holds the gate is silent, and says so.
    inst.gateEnter(10, nullptr);
    inst.mix(out.data(), kFrames);
    inst.gateExit();
    REQUIRE(inst.scopeTap().written() == 5u * kFrames);
    delete bank;
}

TEST_CASE("PitchedInstrument: disabled scope does not capture", "[instrument][scope]") {
    SampleBank* bank = makeBank(4096, 60.0f, 0, 0, constFill);
    PitchedInstrument inst;
    inst.scopeTap().setEnabled(false);
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(24));
    inst.setBank(bank);
    inst.noteOn(60, 127, 0);

    std::vector<float> out(kFrames * 2, 0.0f);
    inst.mix(out.data(), kFrames);

    REQUIRE(inst.scopeTap().written() == 0);
    REQUIRE(std::any_of(out.begin(), out.end(), [](float v) { return std::fabs(v) > 0.0f; }));
    delete bank;
}

TEST_CASE("PitchedInstrument: lastNoteHz follows the notes that sounded",
          "[instrument][scope]") {
    SampleBank* bank = makeBank(4096, 60.0f, 0, 0, constFill);
    PitchedInstrument inst;
    inst.onPrepare(kFrames, kSr);
    inst.setParams(params(24));

    REQUIRE(inst.lastNoteHz() == 0.0f);
    inst.noteOn(69, 127, 0);                       // no bank: nothing sounded
    REQUIRE(inst.lastNoteHz() == 0.0f);

    inst.setBank(bank);
    inst.noteOn(69, 127, 0);
    REQUIRE(inst.lastNoteHz() == Catch::Approx(440.0f).margin(1e-3));
    inst.noteOn(60, 127, 0);
    REQUIRE(inst.lastNoteHz() == Catch::Approx(261.6255653f).margin(1e-3));
    inst.noteOn(81, 127, 0);
    REQUIRE(inst.lastNoteHz() == Catch::Approx(880.0f).margin(1e-3));

    // A note refused while the loader is mutating never sounded either.
    inst.beginMutation();
    inst.noteOn(69, 127, 0);
    inst.endMutation();
    REQUIRE(inst.lastNoteHz() == Catch::Approx(880.0f).margin(1e-3));
    delete bank;
}

TEST_CASE("PitchedInstrument: a crossfaded loop wraps without a seam",
          "[instrument][engine][loop]") {
    // A ramp jumps by the whole loop span at a plain wrap -- the click a held
    // note built from one sample makes every loop. With the crossfade the
    // voice is already playing what precedes the loop start when it wraps.
    constexpr uint32_t kStart = 1000, kEnd = 2000, kXf = 500;
    auto maxStep = [](SampleBank* bank) {
        PitchedInstrument inst;
        inst.onPrepare(kFrames, kSr);
        inst.setParams(params(4, 0.0f, 5000.0f));
        inst.setBank(bank);
        inst.noteOn(60, 127, 0);
        std::vector<float> out(kFrames * 2, 0.0f);
        float prev = 0.0f, worst = 0.0f;
        bool first = true;
        for (int b = 0; b < 100; ++b) {               // 6400 frames: past six wraps
            std::fill(out.begin(), out.end(), 0.0f);
            inst.mix(out.data(), kFrames);
            for (uint32_t k = 0; k < kFrames; ++k) {
                const float v = out[2 * k] * 32768.0f / kCenterGain;
                if (!first) worst = std::max(worst, std::fabs(v - prev));
                prev = v;
                first = false;
            }
        }
        return worst;
    };

    SampleBank* plain = makeBank(4000, 60.0f, kStart, kEnd, rampFill);
    REQUIRE(maxStep(plain) > 900.0f);
    delete plain;

    SampleBank* faded = makeBank(4000, 60.0f, kStart, kEnd, rampFill);
    REQUIRE(faded->editZone(0, 60.0f, kStart, kEnd, kXf));
    REQUIRE(faded->zone(0).loopXfade == kXf);
    REQUIRE(maxStep(faded) < 10.0f);
    delete faded;
}

TEST_CASE("SampleBank: the crossfade and the start stay where they can play",
          "[instrument][loop]") {
    // Never longer than the loop, never reaching before sample 0.
    REQUIRE(SampleBank::loopXfadeFor(1000, 2000, 500) == 500);
    REQUIRE(SampleBank::loopXfadeFor(300, 2000, 500) == 300);
    REQUIRE(SampleBank::loopXfadeFor(3000, 3400, 1000) == 400);
    REQUIRE(SampleBank::loopXfadeFor(0, 0, 500) == 0);        // one-shot

    // A start past the loop would never reach it: the loop start instead.
    REQUIRE(SampleBank::startFor(4000, 1000, 2000, 500) == 500);
    REQUIRE(SampleBank::startFor(4000, 1000, 2000, 2500) == 1000);
    REQUIRE(SampleBank::startFor(4000, 0, 0, 2500) == 2500);  // one-shot: anywhere inside
    REQUIRE(SampleBank::startFor(4000, 0, 0, 4000) == 0);

    SampleBank* bank = makeBank(4000, 60.0f, 300, 2000, rampFill);
    REQUIRE(bank->editZone(0, 60.0f, 300, 2000, 900, 100));
    REQUIRE(bank->zone(0).loopXfade == 300);
    REQUIRE(bank->zone(0).start == 100);
    // Left out, both are kept, and cut again to the new loop.
    REQUIRE(bank->editZone(0, 60.0f, 200, 2000));
    REQUIRE(bank->zone(0).loopXfade == 200);
    REQUIRE(bank->zone(0).start == 100);
    delete bank;
}

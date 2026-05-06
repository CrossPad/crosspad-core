#include <catch2/catch_test_macros.hpp>
#include "crosspad/audio/SynthEngineNode.hpp"
#include "crosspad/synth/ISynthEngine.hpp"

#include <vector>
#include <cstring>

using namespace crosspad;

namespace {

/// Minimal ISynthEngine that writes a constant value into the stereo float buffer.
class ConstSynth : public ISynthEngine {
public:
    explicit ConstSynth(float value) : value_(value) {}

    void process(float* stereoOut, uint32_t frames) override {
        ++processCalls;
        for (uint32_t i = 0; i < frames * 2; ++i) stereoOut[i] = value_;
    }

    int processCalls = 0;

    // ── unused pure-virtuals ──
    void init() override {}
    void cleanup() override {}
    void noteOn(uint8_t, uint8_t) override {}
    void noteOff(uint8_t) override {}
    void setPitchBend(int16_t) override {}
    void setAttack(float) override {}
    void setDecay(float) override {}
    void setSustain(uint8_t) override {}
    uint8_t getSustain() override { return 0; }
    void setRelease(float) override {}
    void setFilterCutoff(float) override {}
    void setFilterReso(float) override {}
    void setWaveform(uint8_t, uint8_t) override {}
    void setOscVolume(uint8_t, float) override {}
    void setOscPitch(uint8_t, float) override {}
    void setDelayEnabled(bool) override {}
    void setDelayTime(float) override {}
    void setDelayFeedback(float) override {}
    void setDelayMix(float) override {}
    void setReverbEnabled(bool) override {}
    void setReverbDecay(float) override {}
    void setReverbMix(float) override {}
    void getLevel(int16_t& l, int16_t& r) override { l = 0; r = 0; }

private:
    float value_;
};

} // namespace

TEST_CASE("SynthEngineNode: identifies as generator", "[audio][node][synth]") {
    SynthEngineNode node;
    REQUIRE(node.isGenerator() == true);
    REQUIRE(std::string(node.name()) == "SynthEngine");
}

TEST_CASE("SynthEngineNode: null engine is no-op", "[audio][node][synth]") {
    SynthEngineNode node;
    std::vector<float> bus(128, 0.7f);    // pre-populated
    node.mix(bus.data(), 64);             // 64 frames * 2 ch
    for (float v : bus) REQUIRE(v == 0.7f);  // unchanged
}

TEST_CASE("SynthEngineNode: mix is additive", "[audio][node][synth]") {
    ConstSynth synth(0.5f);
    SynthEngineNode node(&synth);

    std::vector<float> bus(128, 0.0f);

    SECTION("First call writes synth output") {
        node.mix(bus.data(), 64);
        for (float v : bus) REQUIRE(v == 0.5f);
        REQUIRE(synth.processCalls == 1);
    }

    SECTION("Second call accumulates") {
        node.mix(bus.data(), 64);
        node.mix(bus.data(), 64);
        for (float v : bus) REQUIRE(v == 1.0f);
        REQUIRE(synth.processCalls == 2);
    }

    SECTION("Adds to non-zero baseline") {
        std::fill(bus.begin(), bus.end(), 0.25f);
        node.mix(bus.data(), 64);
        for (float v : bus) REQUIRE(v == 0.75f);
    }
}

TEST_CASE("SynthEngineNode: setEngine swaps backend", "[audio][node][synth]") {
    ConstSynth a(0.1f), b(0.4f);
    SynthEngineNode node(&a);
    REQUIRE(node.getEngine() == &a);

    std::vector<float> bus(128, 0.0f);
    node.mix(bus.data(), 64);
    for (float v : bus) REQUIRE(v == 0.1f);

    node.setEngine(&b);
    REQUIRE(node.getEngine() == &b);

    std::fill(bus.begin(), bus.end(), 0.0f);
    node.mix(bus.data(), 64);
    for (float v : bus) REQUIRE(v == 0.4f);
}

TEST_CASE("SynthEngineNode: scratch buffer grows on demand", "[audio][node][synth]") {
    ConstSynth synth(0.5f);
    SynthEngineNode node(&synth);

    // First call with small frame count, then larger — should not crash or truncate
    std::vector<float> small(64, 0.0f);    // 32 frames
    node.mix(small.data(), 32);
    for (float v : small) REQUIRE(v == 0.5f);

    std::vector<float> large(2048, 0.0f);  // 1024 frames — larger than first call
    node.mix(large.data(), 1024);
    for (float v : large) REQUIRE(v == 0.5f);
}

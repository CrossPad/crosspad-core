// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "crosspad/audio/AbstractAudioModule.hpp"
#include "crosspad/audio/IAudioNode.hpp"

#include <vector>
#include <cstring>

using Catch::Matchers::WithinAbs;
using namespace crosspad;

namespace {

/// Captures samples pushed to it. Reports Int16-only support to exercise the
/// float→int16 conversion path in AbstractAudioModule.
class CapturingInt16Stream : public IAudioStream {
public:
    uint32_t supportedFormats() const override { return audioFormatMask(AudioFormat::Int16); }

    uint32_t pushSamples(const void* samples, AudioFormat fmt, uint32_t frames) override {
        REQUIRE(fmt == AudioFormat::Int16);
        const int16_t* src = static_cast<const int16_t*>(samples);
        const uint32_t n = frames * 2;
        captured.insert(captured.end(), src, src + n);
        ++pushCount;
        return frames;
    }

    bool isOpen() const override { return open; }
    uint32_t getSampleRate() const override { return 48000; }

    bool open = true;
    int  pushCount = 0;
    std::vector<int16_t> captured;
};

/// Captures samples pushed in float format (preferred path).
class CapturingFloatStream : public IAudioStream {
public:
    uint32_t supportedFormats() const override {
        return audioFormatMask(AudioFormat::Float32) | audioFormatMask(AudioFormat::Int16);
    }

    uint32_t pushSamples(const void* samples, AudioFormat fmt, uint32_t frames) override {
        lastFormat = fmt;
        if (fmt == AudioFormat::Float32) {
            const float* src = static_cast<const float*>(samples);
            captured.insert(captured.end(), src, src + frames * 2);
        }
        return frames;
    }

    bool isOpen() const override { return true; }
    uint32_t getSampleRate() const override { return 48000; }

    AudioFormat lastFormat = AudioFormat::Int16;
    std::vector<float> captured;
};

/// Generator node — adds a constant value to bus.
class ConstGenerator : public IAudioNode {
public:
    explicit ConstGenerator(float value) : value_(value) {}
    const char* name() const override { return "ConstGen"; }
    bool isGenerator() const override { return true; }

    void mix(float* out, uint32_t frames) override {
        for (uint32_t i = 0; i < frames * 2; ++i) out[i] += value_;
    }

    void onSampleRateChange(uint32_t sr) override { lastSr = sr; ++srCalls; }

    uint32_t lastSr  = 0;
    int      srCalls = 0;
private:
    float value_;
};

/// Effect node — multiplies bus by a gain in-place.
class GainEffect : public IAudioNode {
public:
    explicit GainEffect(float gain) : gain_(gain) {}
    const char* name() const override { return "Gain"; }

    void process(const float* in, float* out, uint32_t frames) override {
        for (uint32_t i = 0; i < frames * 2; ++i) out[i] = in[i] * gain_;
    }

private:
    float gain_;
};

/// Concrete subclass exposing one capture stream so we can drive process().
class TestAudioModule : public AbstractAudioModule {
public:
    IAudioStream* getOutputStream(uint8_t i) override {
        if (i == 0) return stream0;
        return nullptr;
    }
    IAudioStream* stream0 = nullptr;
};

} // namespace

TEST_CASE("AbstractAudioModule: setup allocates buses", "[audio][module]") {
    TestAudioModule m;
    AudioModuleConfig cfg;
    cfg.sampleRate = 48000;
    cfg.frameCount = 64;
    REQUIRE(m.setup(cfg));
    REQUIRE(m.getConfig().frameCount == 64);
    REQUIRE(m.getConfig().sampleRate == 48000);
}

TEST_CASE("AbstractAudioModule: process pushes silence with no nodes", "[audio][module]") {
    TestAudioModule m;
    CapturingInt16Stream s;
    m.stream0 = &s;

    AudioModuleConfig cfg;
    cfg.frameCount = 32;
    REQUIRE(m.setup(cfg));

    m.process();
    REQUIRE(s.pushCount == 1);
    REQUIRE(s.captured.size() == 32 * 2);
    for (int16_t v : s.captured) REQUIRE(v == 0);
}

TEST_CASE("AbstractAudioModule: generator + effect chain", "[audio][module]") {
    TestAudioModule m;
    CapturingInt16Stream s;
    m.stream0 = &s;

    AudioModuleConfig cfg;
    cfg.frameCount = 32;
    REQUIRE(m.setup(cfg));

    ConstGenerator gen(0.25f);
    GainEffect     halve(0.5f);
    m.addNode(&gen);
    m.addNode(&halve);

    m.process();

    // Bus = 0.25 (generator) → 0.125 (effect) → int16 ≈ 4095
    const int16_t expected = static_cast<int16_t>(0.125f * 32767.0f);
    REQUIRE(s.captured.size() == 32 * 2);
    for (int16_t v : s.captured) {
        REQUIRE(std::abs(static_cast<int>(v - expected)) <= 1);
    }
}

TEST_CASE("AbstractAudioModule: chain order is insertion order", "[audio][module]") {
    TestAudioModule m;
    CapturingInt16Stream s;
    m.stream0 = &s;
    AudioModuleConfig cfg; cfg.frameCount = 16;
    REQUIRE(m.setup(cfg));

    // Insert effect FIRST, then generator. Module dispatches by isGenerator()
    // flag inside the loop, so effect-then-generator means: effect runs on
    // empty bus (no-op), then generator adds 0.5 → final bus = 0.5 (NOT halved).
    GainEffect     halve(0.5f);
    ConstGenerator gen(0.5f);
    m.addNode(&halve);
    m.addNode(&gen);

    m.process();

    const int16_t expected = static_cast<int16_t>(0.5f * 32767.0f);  // 16383
    for (int16_t v : s.captured) {
        REQUIRE(std::abs(static_cast<int>(v - expected)) <= 1);
    }
}

TEST_CASE("AbstractAudioModule: removeNode drops only matching pointer", "[audio][module]") {
    TestAudioModule m;
    AudioModuleConfig cfg; cfg.frameCount = 16;
    REQUIRE(m.setup(cfg));

    ConstGenerator a(0.1f), b(0.2f);
    m.addNode(&a);
    m.addNode(&b);
    REQUIRE(m.nodeCount() == 2);

    m.removeNode(&a);
    REQUIRE(m.nodeCount() == 1);
    REQUIRE(m.getNode(0) == &b);

    // Removing again is harmless
    m.removeNode(&a);
    REQUIRE(m.nodeCount() == 1);
}

TEST_CASE("AbstractAudioModule: onSampleRateChange propagates", "[audio][module]") {
    // AudioModuleConfig default-initializes sampleRate=48000, so addNode()
    // always triggers onSampleRateChange — even before setup() is called.

    SECTION("setup() before addNode — addNode triggers SR callback") {
        TestAudioModule m;
        ConstGenerator a(0.0f);
        AudioModuleConfig cfg; cfg.sampleRate = 96000; cfg.frameCount = 16;
        REQUIRE(m.setup(cfg));
        m.addNode(&a);
        REQUIRE(a.lastSr == 96000);
        REQUIRE(a.srCalls == 1);
    }

    SECTION("addNode before setup — addNode triggers with default 44100, then setup re-triggers") {
        TestAudioModule m;
        ConstGenerator a(0.0f);
        m.addNode(&a);
        REQUIRE(a.lastSr == 44100);   // default from AudioModuleConfig
        REQUIRE(a.srCalls == 1);

        AudioModuleConfig cfg; cfg.sampleRate = 48000; cfg.frameCount = 16;
        REQUIRE(m.setup(cfg));
        REQUIRE(a.lastSr == 48000);
        REQUIRE(a.srCalls == 2);
    }

    SECTION("Re-setup with different SR re-triggers callback") {
        TestAudioModule m;
        ConstGenerator a(0.0f);
        m.addNode(&a);                 // srCalls=1 (default 48000)
        AudioModuleConfig cfg; cfg.sampleRate = 48000; cfg.frameCount = 16;
        REQUIRE(m.setup(cfg));         // srCalls=2 (48000 again)
        cfg.sampleRate = 96000;
        REQUIRE(m.setup(cfg));         // srCalls=3 (96000)
        REQUIRE(a.lastSr == 96000);
        REQUIRE(a.srCalls == 3);
    }
}

TEST_CASE("AbstractAudioModule: onPrepare propagates frameCount", "[audio][module]") {
    // Node that captures both maxFrames and sampleRate from onPrepare().
    class PrepareCapture : public IAudioNode {
    public:
        const char* name() const override { return "PrepareCapture"; }
        bool isGenerator() const override { return true; }
        void onPrepare(uint32_t maxFrames, uint32_t sr) override {
            lastFrames = maxFrames;
            lastSr     = sr;
            ++calls;
        }
        uint32_t lastFrames = 0;
        uint32_t lastSr     = 0;
        int      calls      = 0;
    };

    TestAudioModule m;
    PrepareCapture p;
    AudioModuleConfig cfg; cfg.sampleRate = 96000; cfg.frameCount = 32;
    REQUIRE(m.setup(cfg));
    m.addNode(&p);
    REQUIRE(p.lastFrames == 32);
    REQUIRE(p.lastSr     == 96000);
    REQUIRE(p.calls      == 1);

    // Re-setup with a new frame count must re-fire onPrepare on the node.
    cfg.frameCount = 128;
    REQUIRE(m.setup(cfg));
    REQUIRE(p.lastFrames == 128);
    REQUIRE(p.calls      == 2);
}

TEST_CASE("AbstractAudioModule: peak metering tracks bus", "[audio][module]") {
    TestAudioModule m;
    CapturingInt16Stream s;
    m.stream0 = &s;
    AudioModuleConfig cfg; cfg.frameCount = 16;
    REQUIRE(m.setup(cfg));

    ConstGenerator gen(0.5f);
    m.addNode(&gen);

    m.process();
    float l = 0, r = 0;
    m.getOutputLevel(0, l, r);
    REQUIRE_THAT(l, WithinAbs(0.5f, 0.0001f));
    REQUIRE_THAT(r, WithinAbs(0.5f, 0.0001f));
}

TEST_CASE("AbstractAudioModule: closed stream is skipped", "[audio][module]") {
    TestAudioModule m;
    CapturingInt16Stream s;
    s.open = false;
    m.stream0 = &s;
    AudioModuleConfig cfg; cfg.frameCount = 16;
    REQUIRE(m.setup(cfg));

    m.process();
    REQUIRE(s.pushCount == 0);
}

TEST_CASE("AbstractAudioModule: prefers Float32 stream format", "[audio][module]") {
    // Subclass exposing CapturingFloatStream
    struct FloatModule : public AbstractAudioModule {
        CapturingFloatStream s;
        IAudioStream* getOutputStream(uint8_t i) override { return i == 0 ? &s : nullptr; }
    } m;

    AudioModuleConfig cfg; cfg.frameCount = 16;
    REQUIRE(m.setup(cfg));
    ConstGenerator gen(0.25f);
    m.addNode(&gen);

    m.process();
    REQUIRE(m.s.lastFormat == AudioFormat::Float32);
    REQUIRE(m.s.captured.size() == 16 * 2);
    for (float v : m.s.captured) REQUIRE_THAT(v, WithinAbs(0.25f, 1e-6f));
}

TEST_CASE("AbstractAudioModule: disabled node is skipped", "[audio][module]") {
    TestAudioModule m;
    CapturingInt16Stream s;
    m.stream0 = &s;
    AudioModuleConfig cfg; cfg.frameCount = 16;
    REQUIRE(m.setup(cfg));

    struct DisabledGen : public ConstGenerator {
        DisabledGen() : ConstGenerator(0.5f) {}
        bool enabled() const override { return false; }
    } gen;

    m.addNode(&gen);
    m.process();

    // Disabled generator → bus stays zero
    for (int16_t v : s.captured) REQUIRE(v == 0);
}

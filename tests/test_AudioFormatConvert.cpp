#include <catch2/catch_test_macros.hpp>
#include "crosspad/audio/AudioFormatConvert.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using namespace crosspad;

TEST_CASE("clampFloat: boundary handling", "[audio][convert]") {
    REQUIRE(clampFloat( 0.0f) ==  0.0f);
    REQUIRE(clampFloat( 1.0f) ==  1.0f);
    REQUIRE(clampFloat(-1.0f) == -1.0f);
    REQUIRE(clampFloat( 1.5f) ==  1.0f);
    REQUIRE(clampFloat(-1.5f) == -1.0f);
    REQUIRE(clampFloat( 1000.0f) ==  1.0f);
    REQUIRE(clampFloat(-1000.0f) == -1.0f);

    // NaN → 0 (codified by Tier 1 — silence the sample, never propagate NaN)
    const float nan = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(clampFloat(nan) == 0.0f);

    // Subnormal values pass through unchanged
    const float tiny = 1e-30f;
    REQUIRE(clampFloat(tiny) == tiny);
}

TEST_CASE("floatToInt16: clamps and converts", "[audio][convert]") {
    SECTION("Boundaries") {
        const float in[]  = { 1.5f, -1.5f, 0.0f, 1.0f, -1.0f };
        int16_t out[5] = {};
        floatToInt16(in, out, 5);
        REQUIRE(out[0] == 32767);
        REQUIRE(out[1] == -32767);  // -1.0 * 32767 = -32767 (not -32768; floatToInt16 uses kFloatScaleToInt16)
        REQUIRE(out[2] == 0);
        REQUIRE(out[3] == 32767);
        REQUIRE(out[4] == -32767);
    }
    SECTION("NaN → 0") {
        const float in[]  = { std::numeric_limits<float>::quiet_NaN() };
        int16_t out[1] = { 12345 };
        floatToInt16(in, out, 1);
        REQUIRE(out[0] == 0);
    }
    SECTION("Mid-range") {
        const float in[]  = { 0.5f, -0.5f, 0.25f };
        int16_t out[3] = {};
        floatToInt16(in, out, 3);
        REQUIRE(out[0] == static_cast<int16_t>(0.5f * 32767.0f));   // 16383
        REQUIRE(out[1] == static_cast<int16_t>(-0.5f * 32767.0f));  // -16383
        REQUIRE(out[2] == static_cast<int16_t>(0.25f * 32767.0f));  // 8191
    }
}

TEST_CASE("int16ToFloat: range preserved", "[audio][convert]") {
    const int16_t in[]  = { 0, 32767, -32768, 16384, -16384 };
    float out[5] = {};
    int16ToFloat(in, out, 5);

    REQUIRE(out[0] == 0.0f);
    REQUIRE(out[1] ==  32767.0f / 32768.0f);   // ≈ 0.999969
    REQUIRE(out[2] == -1.0f);                  // -32768/32768 == -1.0
    REQUIRE(out[3] ==  0.5f);
    REQUIRE(out[4] == -0.5f);
}

TEST_CASE("int16 → float → int16 round-trip", "[audio][convert]") {
    SECTION("Spot values within 1 LSB") {
        const std::array<int16_t, 7> in = { -32768, -16384, -1000, 0, 1000, 16384, 32767 };
        std::array<float,   7> mid = {};
        std::array<int16_t, 7> out = {};

        int16ToFloat(in.data(), mid.data(), 7);
        floatToInt16(mid.data(), out.data(), 7);

        for (size_t i = 0; i < in.size(); ++i) {
            const int diff = std::abs(static_cast<int>(in[i]) - static_cast<int>(out[i]));
            INFO("i=" << i << " in=" << in[i] << " out=" << out[i]);
            REQUIRE(diff <= 1);
        }
    }
    SECTION("1024 random samples within 1 LSB") {
        std::mt19937 rng(0xC0FFEE);
        std::uniform_int_distribution<int> dist(-32768, 32767);

        std::vector<int16_t> in(1024);
        for (auto& s : in) s = static_cast<int16_t>(dist(rng));

        std::vector<float>   mid(1024);
        std::vector<int16_t> out(1024);

        int16ToFloat(in.data(), mid.data(), 1024);
        floatToInt16(mid.data(), out.data(), 1024);

        for (size_t i = 0; i < 1024; ++i) {
            const int diff = std::abs(static_cast<int>(in[i]) - static_cast<int>(out[i]));
            REQUIRE(diff <= 1);
        }
    }
}

TEST_CASE("int32 → float → int32 round-trip", "[audio][convert]") {
    // float32 has 24-bit mantissa, so int32 cannot round-trip exactly at the
    // top of its range. Worse: at INT32_MAX/MIN the float→int32 cast hits
    // implementation-defined overflow behavior. Test only mid-range values
    // that fit comfortably in 24 bits scaled by the kFloatScaleToInt32 path.
    const std::array<int32_t, 5> in = {
        0,
        1 << 24,
        -(1 << 24),
        (1 << 30),
        -(1 << 30),
    };
    std::array<float,   5> mid = {};
    std::array<int32_t, 5> out = {};

    int32ToFloat(in.data(), mid.data(), 5);
    floatToInt32(mid.data(), out.data(), 5);

    // At ±2^30, the ULP at full scale is ~256.
    constexpr int kTol = 256;
    for (size_t i = 0; i < in.size(); ++i) {
        const int64_t diff = std::abs(static_cast<int64_t>(in[i]) - static_cast<int64_t>(out[i]));
        INFO("i=" << i << " in=" << in[i] << " out=" << out[i] << " diff=" << diff);
        REQUIRE(diff <= kTol);
    }
}

TEST_CASE("convertToFloat: dispatch by AudioFormat tag", "[audio][convert]") {
    SECTION("Float32 → memcpy") {
        const float src[] = { 0.25f, -0.75f, 1.0f };
        float dst[3] = {};
        convertToFloat(src, AudioFormat::Float32, dst, 3);
        REQUIRE(dst[0] == src[0]);
        REQUIRE(dst[1] == src[1]);
        REQUIRE(dst[2] == src[2]);
    }
    SECTION("Int16 → float") {
        const int16_t src[] = { 16384, -16384 };
        float dst[2] = {};
        convertToFloat(src, AudioFormat::Int16, dst, 2);
        REQUIRE(dst[0] ==  0.5f);
        REQUIRE(dst[1] == -0.5f);
    }
    SECTION("Int32 → float") {
        const int32_t src[] = { 0, 1 << 30 };
        float dst[2] = {};
        convertToFloat(src, AudioFormat::Int32, dst, 2);
        REQUIRE(dst[0] == 0.0f);
        REQUIRE(dst[1] == 0.5f);
    }
}

TEST_CASE("convertFromFloat: dispatch by AudioFormat tag", "[audio][convert]") {
    SECTION("Float32 → memcpy") {
        const float src[] = { 0.1f, -0.2f };
        float dst[2] = {};
        convertFromFloat(src, dst, AudioFormat::Float32, 2);
        REQUIRE(dst[0] == src[0]);
        REQUIRE(dst[1] == src[1]);
    }
    SECTION("Float → int16") {
        const float src[] = { 0.5f, -0.5f, 1.5f /* clamps */ };
        int16_t dst[3] = {};
        convertFromFloat(src, dst, AudioFormat::Int16, 3);
        REQUIRE(dst[0] == static_cast<int16_t>(0.5f * 32767.0f));
        REQUIRE(dst[1] == static_cast<int16_t>(-0.5f * 32767.0f));
        REQUIRE(dst[2] == 32767);
    }
    SECTION("Float → int32") {
        const float src[] = { 0.5f, -0.5f };
        int32_t dst[2] = {};
        convertFromFloat(src, dst, AudioFormat::Int32, 2);
        REQUIRE(dst[0] == static_cast<int32_t>(0.5f  * 2147483647.0f));
        REQUIRE(dst[1] == static_cast<int32_t>(-0.5f * 2147483647.0f));
    }
}

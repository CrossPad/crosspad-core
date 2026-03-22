#include <catch2/catch_test_macros.hpp>
#include "crosspad/audio/AudioRingBuffer.hpp"

using namespace crosspad;

TEST_CASE("AudioRingBuffer: empty buffer", "[audio]") {
    AudioRingBuffer<float> buf(64);
    REQUIRE(buf.available() == 0);
    REQUIRE(buf.space() == 63); // capacity - 1

    float out[4];
    REQUIRE(buf.read(out, 4) == 0);
}

TEST_CASE("AudioRingBuffer: write and read", "[audio]") {
    AudioRingBuffer<float> buf(64);

    float data[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    REQUIRE(buf.write(data, 4) == 4);
    REQUIRE(buf.available() == 4);

    float out[4] = {};
    REQUIRE(buf.read(out, 4) == 4);
    REQUIRE(out[0] == 1.0f);
    REQUIRE(out[1] == 2.0f);
    REQUIRE(out[2] == 3.0f);
    REQUIRE(out[3] == 4.0f);
    REQUIRE(buf.available() == 0);
}

TEST_CASE("AudioRingBuffer: partial read", "[audio]") {
    AudioRingBuffer<float> buf(64);

    float data[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    buf.write(data, 8);

    float out[4] = {};
    REQUIRE(buf.read(out, 4) == 4);
    REQUIRE(out[0] == 0.0f);
    REQUIRE(out[3] == 3.0f);
    REQUIRE(buf.available() == 4);

    REQUIRE(buf.read(out, 4) == 4);
    REQUIRE(out[0] == 4.0f);
    REQUIRE(out[3] == 7.0f);
}

TEST_CASE("AudioRingBuffer: full buffer clips write", "[audio]") {
    AudioRingBuffer<int> buf(8); // capacity 8, usable 7

    int data[7] = {1, 2, 3, 4, 5, 6, 7};
    REQUIRE(buf.write(data, 7) == 7);
    REQUIRE(buf.space() == 0);

    // Additional write returns 0
    int extra = 99;
    REQUIRE(buf.write(&extra, 1) == 0);
}

TEST_CASE("AudioRingBuffer: wrap-around", "[audio]") {
    AudioRingBuffer<int> buf(8);

    // Fill and drain to advance pointers near the end
    int data[5] = {10, 20, 30, 40, 50};
    buf.write(data, 5);
    int tmp[5];
    buf.read(tmp, 5);

    // Now write across boundary
    int wrap[6] = {1, 2, 3, 4, 5, 6};
    REQUIRE(buf.write(wrap, 6) == 6);

    int out[6];
    REQUIRE(buf.read(out, 6) == 6);
    for (int i = 0; i < 6; i++) {
        REQUIRE(out[i] == i + 1);
    }
}

TEST_CASE("AudioRingBuffer: reset clears state", "[audio]") {
    AudioRingBuffer<float> buf(32);

    float data[10] = {};
    buf.write(data, 10);
    REQUIRE(buf.available() == 10);

    buf.reset();
    REQUIRE(buf.available() == 0);
    REQUIRE(buf.space() == 31);
}

TEST_CASE("AudioRingBuffer: resize", "[audio]") {
    AudioRingBuffer<int> buf;

    // Zero capacity — write does nothing
    int data = 42;
    REQUIRE(buf.write(&data, 1) == 0);

    buf.resize(16);
    REQUIRE(buf.write(&data, 1) == 1);
    REQUIRE(buf.available() == 1);
}

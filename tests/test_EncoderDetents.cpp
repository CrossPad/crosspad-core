#include <catch2/catch_test_macros.hpp>

#include "crosspad/stm32/EncoderDetents.hpp"

using namespace crosspad;

namespace {

/// A report the way the STM sends it, from its LPTIM count.
struct Report { uint8_t top, phase; };

Report at(int cnt, bool settled)
{
    cnt &= 63;
    return {static_cast<uint8_t>(((cnt + 1) >> 2) & 0x0F),
            static_cast<uint8_t>((cnt & 3) | (settled ? EncoderDetents::kSettled : 0))};
}

int feed(EncoderDetents& e, int cnt, bool settled)
{
    const Report r = at(cnt, settled);
    return e.update(r.top, r.phase);
}

} // namespace

TEST_CASE("encoder: one click is one step, each way", "[encoder]") {
    EncoderDetents e;
    CHECK(feed(e, 58, true) == 0);           // at rest, count 2 mod 4
    int steps = 0;
    for (int c : {59, 60, 61}) steps += feed(e, c, false);
    steps += feed(e, 62, true);
    CHECK(steps == 1);
    steps = 0;
    for (int c : {61, 60, 59}) steps += feed(e, c, false);
    steps += feed(e, 58, true);
    CHECK(steps == -1);
}

TEST_CASE("encoder: a hair past the detent and back is nothing", "[encoder]") {
    EncoderDetents e;
    feed(e, 58, true);
    /* The STM's index flips between 58 and 59: this used to be a step there
     * and a step back. */
    CHECK(feed(e, 59, false) == 0);
    CHECK(feed(e, 58, true) == 0);
    CHECK(feed(e, 57, false) == 0);
    CHECK(feed(e, 56, false) == 0);
    CHECK(feed(e, 58, true) == 0);
}

TEST_CASE("encoder: the step comes three counts into the next detent", "[encoder]") {
    EncoderDetents e;
    feed(e, 10, true);
    CHECK(feed(e, 12, false) == 0);
    CHECK(feed(e, 13, false) == 1);
    CHECK(feed(e, 14, true) == 0);           // arriving is not a second step
}

TEST_CASE("encoder: a fast turn read between reports keeps every detent", "[encoder]") {
    EncoderDetents e;
    feed(e, 6, true);
    CHECK(feed(e, 17, false) == 3);           // 11 counts on: three detents, a fourth not yet
    CHECK(feed(e, 22, true) == 1);
    CHECK(feed(e, 10, true) == -3);
}

TEST_CASE("encoder: nothing counts before the knob is first seen at rest", "[encoder]") {
    EncoderDetents e;
    CHECK(e.update(0, 0) == 0);               // no report yet: all zero
    CHECK(feed(e, 58, true) == 0);            // the first report, at rest: where it is
    CHECK(feed(e, 58, true) == 0);
    CHECK(feed(e, 62, true) == 1);
}

TEST_CASE("encoder: across the wrap of the count", "[encoder]") {
    EncoderDetents e;
    feed(e, 62, true);
    CHECK(feed(e, 1, false) == 1);            // 62 -> 65, which is 1
    CHECK(feed(e, 2, true) == 0);
    CHECK(feed(e, 62, true) == -1);
}

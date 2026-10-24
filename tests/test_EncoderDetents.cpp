// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "crosspad/stm32/EncoderDetents.hpp"

#include <initializer_list>

using namespace crosspad;

namespace {

/// A report the way the STM sends it: its count mod 4 and the live pins.
uint8_t report(int cnt, int a, int b)
{
    return static_cast<uint8_t>((cnt & 3) | (a << 2) | (b << 3) | (a && b ? 0x10 : 0));
}

/// The pins at a position in the detent's cycle, the way the count rises:
/// 0 = (1,1) rest, 1 = (0,1), 2 = (0,0), 3 = (1,0).
uint8_t pins(int at)
{
    static const int kA[4] = {1, 0, 0, 1};
    static const int kB[4] = {1, 1, 0, 0};
    at &= 3;
    return report(at + 1, kA[at], kB[at]);   // a count that tells the truth
}

int feed(EncoderDetents& e, std::initializer_list<int> ats)
{
    int steps = 0;
    for (int at : ats) steps += e.update(pins(at));
    return steps;
}

} // namespace

TEST_CASE("encoder: one click is one step, each way", "[encoder]") {
    EncoderDetents e;
    CHECK(feed(e, {0}) == 0);
    CHECK(feed(e, {1, 2, 3, 0}) == 1);
    CHECK(feed(e, {3, 2, 1, 0}) == -1);
}

TEST_CASE("encoder: the step comes three states in, landing is not another", "[encoder]") {
    EncoderDetents e;
    feed(e, {0});
    CHECK(feed(e, {1, 2}) == 0);
    CHECK(feed(e, {3}) == 1);
    CHECK(feed(e, {0}) == 0);
    CHECK(feed(e, {1, 0}) == 0);              // the spring's overshoot and back
}

TEST_CASE("encoder: a wobble in the detent is nothing", "[encoder]") {
    EncoderDetents e;
    feed(e, {0});
    CHECK(feed(e, {1, 0, 3, 0, 1, 2, 1, 0}) == 0);
}

TEST_CASE("encoder: going back two states undoes a step, one does not", "[encoder]") {
    EncoderDetents e;
    feed(e, {0});
    CHECK(feed(e, {1, 2, 3}) == 1);
    CHECK(feed(e, {2, 3}) == 0);              // a bounce at the edge
    CHECK(feed(e, {2, 1}) == -1);             // turned back after all
    CHECK(feed(e, {0}) == 0);
}

TEST_CASE("encoder: a count that slips is ignored -- one click measured on the board", "[encoder]") {
    /* 41.5 s into a capture: the pins made one Gray cycle while the LPTIM
     * went 1, 0, 60, 59, 58 -- seven counts, four of them at one edge. The
     * count-based decoder made that two steps. */
    EncoderDetents e;
    CHECK(e.update(report(1, 1, 1)) == 0);
    int steps = 0;
    steps += e.update(report(0, 1, 0));
    steps += e.update(report(60, 0, 0));
    steps += e.update(report(59, 0, 1));
    steps += e.update(report(58, 1, 1));
    CHECK(steps == -1);
}

TEST_CASE("encoder: a missed report keeps the direction it was going", "[encoder]") {
    EncoderDetents e;
    feed(e, {0});
    CHECK(feed(e, {1, 3}) == 1);              // (0,0) never reported
    CHECK(feed(e, {0}) == 0);
    CHECK(feed(e, {3, 1}) == -1);
    CHECK(feed(e, {0}) == 0);
}

TEST_CASE("encoder: a fast turn keeps every detent", "[encoder]") {
    EncoderDetents e;
    feed(e, {0});
    CHECK(feed(e, {1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0}) == 3);
    CHECK(feed(e, {3, 2, 1, 0, 3, 2, 1, 0}) == -2);
}

TEST_CASE("encoder: the first report is where it is", "[encoder]") {
    EncoderDetents e;
    CHECK(feed(e, {2}) == 0);                 // between detents at boot
    CHECK(feed(e, {3, 0}) == 0);              // two states: into the detent it was near
    CHECK(feed(e, {1, 2, 3}) == 1);
}

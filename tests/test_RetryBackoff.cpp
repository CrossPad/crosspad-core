#include <catch2/catch_test_macros.hpp>

#include "crosspad/util/RetryBackoff.hpp"

using crosspad::RetryBackoff;

TEST_CASE("retry backoff: at once, then twice as long after each failure, at most a minute", "[util]") {
    RetryBackoff b;
    CHECK(b.due(0));
    b.failed(1000);
    CHECK_FALSE(b.due(2999));
    CHECK(b.due(3000));                                    // 2 s
    b.failed(3000);
    CHECK_FALSE(b.due(6999));
    CHECK(b.due(7000));                                    // 4 s
    for (int i = 0; i < 10; ++i) b.failed(7000);
    CHECK_FALSE(b.due(7000 + RetryBackoff::kMostMs - 1));
    CHECK(b.due(7000 + RetryBackoff::kMostMs));
    b.reset();
    CHECK(b.due(7000));
}

TEST_CASE("retry backoff: the clock wrapping round does not stall it", "[util]") {
    RetryBackoff b;
    b.failed(0xFFFFFC00u);                                 // due 2 s later, past the wrap (0x3D0)
    CHECK_FALSE(b.due(0xFFFFFF00u));
    CHECK_FALSE(b.due(0x000003CFu));
    CHECK(b.due(0x000003D0u));
}

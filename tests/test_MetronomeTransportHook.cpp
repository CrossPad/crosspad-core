// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include "crosspad/sequencer/MetronomeNode.hpp"

#include <vector>

using namespace crosspad;

TEST_CASE("metronome transport hook fires on start and stop, not on resume", "[metronome]") {
    MetronomeNode m;
    std::vector<int> seen;
    m.setTransportHook([](bool running, void * ctx) {
        static_cast<std::vector<int> *>(ctx)->push_back(running ? 1 : 0);
    }, &seen);
    m.start();
    m.resume();
    m.stop();
    REQUIRE(seen == std::vector<int>{1, 0});
}

#include <catch2/catch_test_macros.hpp>

#include "crosspad/audio/SpeakerAmpGate.hpp"

using crosspad::SpeakerAmpGate;

TEST_CASE("SpeakerAmpGate::signalWithin follows the output, not the speaker switch", "[audio][amp]") {
    SpeakerAmpGate g;
    REQUIRE_FALSE(g.signalWithin(0, 2000));          // nothing heard yet

    g.setAllowed(false);                              // headphones only
    g.submitLevel(0.5f);
    REQUIRE_FALSE(g.poll(1000));                      // amp stays off...
    REQUIRE(g.signalWithin(1000, 2000));              // ...but there is signal
    REQUIRE(g.signalWithin(3000, 2000));
    REQUIRE_FALSE(g.signalWithin(3001, 2000));        // silent long enough

    g.submitLevel(0.001f);                            // below offThreshold: silence
    g.poll(4000);
    REQUIRE_FALSE(g.signalWithin(4000, 2000));
}

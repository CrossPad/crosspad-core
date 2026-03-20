#include <catch2/catch_test_macros.hpp>
#include "crosspad/platform/PlatformCapabilities.hpp"

using namespace crosspad;

TEST_CASE("PlatformCapabilities: starts empty", "[capabilities]") {
    setPlatformCapabilities(Capability::None);
    REQUIRE_FALSE(hasCapability(Capability::Midi));
    REQUIRE_FALSE(hasAnyCapability(Capability::Midi | Capability::Pads));
}

TEST_CASE("PlatformCapabilities: set and query", "[capabilities]") {
    setPlatformCapabilities(Capability::Midi | Capability::Pads);

    REQUIRE(hasCapability(Capability::Midi));
    REQUIRE(hasCapability(Capability::Pads));
    REQUIRE_FALSE(hasCapability(Capability::AudioOut));

    // hasCapability requires ALL flags
    REQUIRE(hasCapability(Capability::Midi | Capability::Pads));
    REQUIRE_FALSE(hasCapability(Capability::Midi | Capability::AudioOut));

    // hasAnyCapability requires ANY flag
    REQUIRE(hasAnyCapability(Capability::Midi | Capability::AudioOut));
    REQUIRE_FALSE(hasAnyCapability(Capability::AudioOut | Capability::WiFi));

    // cleanup
    setPlatformCapabilities(Capability::None);
}

TEST_CASE("PlatformCapabilities: add and remove", "[capabilities]") {
    setPlatformCapabilities(Capability::None);

    addPlatformCapability(Capability::Display);
    REQUIRE(hasCapability(Capability::Display));

    addPlatformCapability(Capability::Encoder);
    REQUIRE(hasCapability(Capability::Display | Capability::Encoder));

    removePlatformCapability(Capability::Display);
    REQUIRE_FALSE(hasCapability(Capability::Display));
    REQUIRE(hasCapability(Capability::Encoder));

    // cleanup
    setPlatformCapabilities(Capability::None);
}

TEST_CASE("PlatformCapabilities: getPlatformCapabilities roundtrip", "[capabilities]") {
    auto caps = Capability::Midi | Capability::Pads | Capability::Leds;
    setPlatformCapabilities(caps);
    REQUIRE(getPlatformCapabilities() == caps);

    setPlatformCapabilities(Capability::None);
}

#include <catch2/catch_test_macros.hpp>
#include "crosspad/stm32/Stm32NoteMap.hpp"

using namespace crosspad;

TEST_CASE("stm32NoteToPad maps note to pad honoring offset", "[stm][notemap]") {
    REQUIRE(stm32NoteToPad(36, 0) == 0);
    REQUIRE(stm32NoteToPad(51, 0) == 15);
    REQUIRE(stm32NoteToPad(35, 0) == 0xFF);   // below range
    REQUIRE(stm32NoteToPad(52, 0) == 0xFF);   // above range

    // offset +12: STM emits 36+pad+12, so pad0 -> note 48
    REQUIRE(stm32NoteToPad(48, 12) == 0);
    REQUIRE(stm32NoteToPad(63, 12) == 15);
    REQUIRE(stm32NoteToPad(36, 12) == 0xFF);  // now below shifted base

    // offset -12: pad0 -> note 24
    REQUIRE(stm32NoteToPad(24, -12) == 0);
    REQUIRE(stm32NoteToPad(39, -12) == 15);
}

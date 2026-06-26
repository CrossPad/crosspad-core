#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"
#include "crosspad/stm32/Stm32ConfigSync.hpp"
#include "crosspad/stm32/reg_map.h"

using namespace crosspad;
using namespace test;

TEST_CASE("stm32ShouldResync: first sync and reset detection", "[stm][sync]") {
    REQUIRE(stm32ShouldResync(-1, 7) == true);    // never synced
    REQUIRE(stm32ShouldResync(7, 7) == false);    // unchanged
    REQUIRE(stm32ShouldResync(7, 8) == true);     // STM rebooted
    REQUIRE(stm32ShouldResync(255, 0) == true);   // wrap is still a change
}

TEST_CASE("Stm32ConfigSync::pushAll writes the ESP-authoritative set", "[stm][sync]") {
    MockStm32Bus bus;
    Stm32Settings s;                 // defaults
    s.midiChannel = 9;
    s.noteOffset = -5;
    s.sleepTimeoutS = 1234;          // 0x04D2
    s.padAnalogMode[2] = 1;

    Stm32ConfigSync sync(bus);
    REQUIRE(sync.pushAll(s) == true);

    // system bytes
    REQUIRE(bus.lastByteAt(REG_CTL_MIDI_CHAN) == 9);
    REQUIRE(bus.lastByteAt(REG_CTL_NOTE_OFFSET) == (uint8_t)(int8_t)-5);
    REQUIRE(bus.lastByteAt(REG_CTL_KEYPAD_DIS) == 0);  // keypadEnabled=true -> dis=0

    // 16-bit timeout written HI then LO
    REQUIRE(bus.lastByteAt(REG_CTL_SLEEP_TO_S_HI) == 0x04);
    REQUIRE(bus.lastByteAt(REG_CTL_SLEEP_TO_S_LO) == 0xD2);

    // charger registers (0x0320..REG_CTL_VEL_CURVE_DEF-1) must NOT be touched,
    // and pad-mapping block must NOT be touched
    for (const auto& w : bus.writes) {
        bool inChargerRange = (w.reg >= REG_CTL_CHG_ICHG_HI && w.reg < REG_CTL_VEL_CURVE_DEF);
        REQUIRE_FALSE(inChargerRange);
        REQUIRE_FALSE((w.reg >= REG_MAPPING_BASE && w.reg < REG_MAPPING_BASE + REG_MAPPING_LEN));
    }

    // per-pad config block: pad 2 analog byte at PAD_CFG_BASE + 2*6 + FIELD_ANALOG
    uint16_t pad2 = REG_PAD_CFG_BASE + 2 * REG_PAD_CFG_STRIDE;
    bool foundPad2 = false;
    for (const auto& w : bus.writes)
        if (w.reg == pad2 && w.data.size() == REG_PAD_CFG_STRIDE) {
            REQUIRE(w.data[REG_PAD_CFG_FIELD_ANALOG] == 1);
            foundPad2 = true;
        }
    REQUIRE(foundPad2);
}

TEST_CASE("Stm32ConfigSync::readBootCount", "[stm][sync]") {
    MockStm32Bus bus;
    bus.readValues[REG_IDENT_BOOT_COUNT] = 42;
    Stm32ConfigSync sync(bus);
    uint8_t bc = 0;
    REQUIRE(sync.readBootCount(bc) == true);
    REQUIRE(bc == 42);
}

TEST_CASE("Stm32ConfigSync per-field writes", "[stm][sync]") {
    test::MockStm32Bus bus;
    Stm32ConfigSync sync(bus);

    REQUIRE(sync.writeReg8(REG_CTL_MIDI_CHAN, 7) == true);
    REQUIRE(bus.lastByteAt(REG_CTL_MIDI_CHAN) == 7);

    REQUIRE(sync.writeReg16(REG_CTL_SLEEP_TO_S_HI, 0x0102) == true);
    REQUIRE(bus.lastByteAt(REG_CTL_SLEEP_TO_S_HI) == 0x01);
    REQUIRE(bus.lastByteAt(REG_CTL_SLEEP_TO_S_LO) == 0x02);
}

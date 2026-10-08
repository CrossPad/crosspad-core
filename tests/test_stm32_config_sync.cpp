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
    s.analogMode(2) = 1;

    Stm32ConfigSync sync(bus);
    REQUIRE(sync.pushAll(s) == true);

    // system bytes
    REQUIRE(bus.lastByteAt(REG_CTL_MIDI_CHAN) == 9);
    REQUIRE(bus.lastByteAt(REG_CTL_NOTE_OFFSET) == (uint8_t)(int8_t)-5);
    REQUIRE(bus.lastByteAt(REG_CTL_KEYPAD_DIS) == 0);  // keypadEnabled=true -> dis=0

    // 16-bit timeout written HI then LO
    REQUIRE(bus.lastByteAt(REG_CTL_SLEEP_TO_S_HI) == 0x04);
    REQUIRE(bus.lastByteAt(REG_CTL_SLEEP_TO_S_LO) == 0xD2);

    // Charger and button globals (0x0320..REG_CTL_VEL_CURVE_DEF-1) must NOT be
    // touched, press debounce excepted: the note-on window is the ESP's.
    // In the pad-mapping block only the swap field is ours (core 1.17.1): the
    // cell binding (mux/channel) stays the STM's, which cell is "left" is not.
    REQUIRE(bus.lastByteAt(REG_CTL_BTN_PRESS_DB_MS) == BOARD_PRESS_DEBOUNCE_MS_DEF);
    int swapWrites = 0;
    for (const auto& w : bus.writes) {
        bool inChargerRange = (w.reg >= REG_CTL_CHG_ICHG_HI && w.reg < REG_CTL_VEL_CURVE_DEF &&
                               w.reg != REG_CTL_BTN_PRESS_DB_MS);
        REQUIRE_FALSE(inChargerRange);
        if (w.reg >= REG_MAPPING_BASE && w.reg < REG_MAPPING_BASE + REG_MAPPING_LEN) {
            uint16_t off = (uint16_t)(w.reg - REG_MAPPING_BASE);
            REQUIRE(off % REG_MAPPING_STRIDE == REG_MAPPING_FIELD_SWAP);
            REQUIRE(w.data.size() == 1);
            int pad = off / REG_MAPPING_STRIDE;
            REQUIRE(w.data[0] == ((s.padPanSwap >> pad) & 1u));
            ++swapWrites;
        }
    }
    REQUIRE(swapWrites == 16);

    // The ESP rail is the STM's to hold: pushAll never writes EN_3V3_HOLD.
    for (const auto& w : bus.writes) REQUIRE(w.reg != REG_CTL_EN_3V3_HOLD);
    REQUIRE(bus.lastByteAt(REG_MAPPING_BASE + 6 * REG_MAPPING_STRIDE + REG_MAPPING_FIELD_SWAP) == 1);  // pad 6 default

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

TEST_CASE("Stm32ConfigSync::pushAll carries the press debounce the unit keeps", "[stm][sync]") {
    MockStm32Bus bus;
    Stm32Settings s;
    s.pressDebounceMs = 9;
    Stm32ConfigSync sync(bus);
    REQUIRE(sync.pushAll(s) == true);
    REQUIRE(bus.lastByteAt(REG_CTL_BTN_PRESS_DB_MS) == 9);
    for (const auto& w : bus.writes) REQUIRE(w.reg != REG_CTL_BTN_RELEASE_DB_MS);
}

TEST_CASE("Stm32ConfigSync::readBootCount", "[stm][sync]") {
    MockStm32Bus bus;
    bus.readValues[REG_IDENT_BOOT_COUNT] = 42;
    Stm32ConfigSync sync(bus);
    uint8_t bc = 0;
    REQUIRE(sync.readBootCount(bc) == true);
    REQUIRE(bc == 42);
}

TEST_CASE("Stm32ConfigSync::readPadVelRaw reads the right register, bounds-checks", "[stm][sync]") {
    test::MockStm32Bus bus;
    bus.readValues[REG_IN_PAD_VELRAW_BASE + 7] = 73;
    Stm32ConfigSync sync(bus);
    uint8_t v = 0;
    REQUIRE(sync.readPadVelRaw(7, v) == true);
    REQUIRE(v == 73);
    REQUIRE(sync.readPadVelRaw(16, v) == false); // out of range (kPads == 16)
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

TEST_CASE("Stm32ConfigSync::writePadConfig writes one pad's 6-byte block", "[stm][sync]") {
    test::MockStm32Bus bus;
    Stm32Settings s;
    s.sensitivity(5) = 0x0123;
    s.lrRatio(5)     = 40;
    s.analogMode(5)  = 3;
    s.velCurve(5)    = 1;
    s.velFloor(5)    = 20;

    Stm32ConfigSync sync(bus);
    REQUIRE(sync.writePadConfig(5, s) == true);

    uint16_t base = REG_PAD_CFG_BASE + 5 * REG_PAD_CFG_STRIDE;
    bool found = false;
    for (const auto& w : bus.writes) {
        if (w.reg == base && w.data.size() == REG_PAD_CFG_STRIDE) {
            REQUIRE(w.data[REG_PAD_CFG_FIELD_SENS_HI]   == 0x01);
            REQUIRE(w.data[REG_PAD_CFG_FIELD_SENS_LO]   == 0x23);
            REQUIRE(w.data[REG_PAD_CFG_FIELD_LR_RATIO]  == 40);
            REQUIRE(w.data[REG_PAD_CFG_FIELD_ANALOG]    == 3);
            REQUIRE(w.data[REG_PAD_CFG_FIELD_VEL_CURVE] == 1);
            REQUIRE(w.data[REG_PAD_CFG_FIELD_VEL_FLOOR] == 20);
            found = true;
        }
    }
    REQUIRE(found);
    REQUIRE(sync.writePadConfig(99, s) == false); // out of range
}

TEST_CASE("Stm32ConfigSync::readPadLive unpacks A/B and reads sum/pan/velraw", "[stm][sync]") {
    test::MockStm32Bus bus;
    const uint8_t pad = 4;
    bus.readValues[REG_IN_PAD_SUM_BASE + pad]    = 90;
    bus.readValues[REG_IN_PAD_DIFF_BASE + pad]   = (uint8_t)(int8_t)-20;
    bus.readValues[REG_IN_PAD_VELRAW_BASE + pad] = 77;
    // rawA=0xABC (2748), rawB=0x123 (291): b0=0xAB, b1=0xC1, b2=0x23
    uint16_t abBase = (uint16_t)(REG_IN_PAD_AB_BASE + pad * 3);
    bus.readValues[abBase + 0] = 0xAB;
    bus.readValues[abBase + 1] = 0xC1;
    bus.readValues[abBase + 2] = 0x23;
    Stm32ConfigSync sync(bus);
    Stm32PadLive lv;
    REQUIRE(sync.readPadLive(pad, lv) == true);
    REQUIRE(lv.sum == 90);
    REQUIRE(lv.pan == -20);
    REQUIRE(lv.velRaw == 77);
    REQUIRE(lv.rawA == 0xABC);
    REQUIRE(lv.rawB == 0x123);
    REQUIRE(sync.readPadLive(16, lv) == false);
}

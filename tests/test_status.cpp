#include <catch2/catch_test_macros.hpp>
#include "crosspad/status/CrosspadStatus.hpp"

using namespace crosspad;

TEST_CASE("Stm32Status: battery percentage calculation", "[status]") {
    SECTION("below minimum returns -1") {
        REQUIRE(Stm32Status::calculateBatteryPct(3100) == -1);
        REQUIRE(Stm32Status::calculateBatteryPct(0) == -1);
    }

    SECTION("at minimum returns 0") {
        REQUIRE(Stm32Status::calculateBatteryPct(3300) == 0);
    }

    SECTION("at maximum returns 100") {
        REQUIRE(Stm32Status::calculateBatteryPct(4200) == 100);
        REQUIRE(Stm32Status::calculateBatteryPct(4500) == 100); // above max
    }

    SECTION("known curve points") {
        REQUIRE(Stm32Status::calculateBatteryPct(3500) == 5);
        REQUIRE(Stm32Status::calculateBatteryPct(3600) == 10);
        REQUIRE(Stm32Status::calculateBatteryPct(3750) == 40);
        REQUIRE(Stm32Status::calculateBatteryPct(4050) == 90);
    }

    SECTION("interpolation between curve points") {
        int8_t pct = Stm32Status::calculateBatteryPct(3550); // between 3500(5%) and 3600(10%)
        REQUIRE(pct >= 5);
        REQUIRE(pct <= 10);
    }
}

TEST_CASE("Stm32Status: register parsing", "[status]") {
    Stm32Status status;
    status.SetStm32Status(0x05, 0x02); // usbChargingBusy + usbStatus, isAnyPressed

    REQUIRE(status.usbChargingBusy == true);
    REQUIRE(status.usbStatus == true);
    REQUIRE(status.batteryFault == false);
    REQUIRE(status.isAnyPressed == true);
    REQUIRE(status.encoderDirection == false);
    REQUIRE(status.isDfuMode == false);
}

TEST_CASE("Stm32Status: voltage unpacking", "[status]") {
    Stm32Status status;
    // Pack 5000mV (0x2710) as 14-bit: high = (5000>>7)&0x7F = 39, low = 5000&0x7F = 8
    uint8_t data[6];
    uint16_t v = 5000;
    data[0] = (v >> 7) & 0x7F;
    data[1] = v & 0x7F;
    data[2] = data[0]; // same value for ESP
    data[3] = data[1];
    data[4] = data[0]; // same value for BAT
    data[5] = data[1];

    status.unpackVoltages(data);
    REQUIRE(status.VUSB_STM == v);
    REQUIRE(status.VUSB_ESP == v);
    REQUIRE(status.VBAT == v);
}

TEST_CASE("CrosspadStatus: GUI state transitions", "[status]") {
    CrosspadStatus status;

    REQUIRE(status.SetCurrentGUI(CrosspadStatus::GUI_MAIN));
    REQUIRE(status.currentGUI == CrosspadStatus::GUI_MAIN);

    REQUIRE(status.SetCurrentGUI(CrosspadStatus::GUI_SETTINGS));
    REQUIRE(status.currentGUI == CrosspadStatus::GUI_SETTINGS);

    // Can't go back to WELCOME
    REQUIRE_FALSE(status.SetCurrentGUI(CrosspadStatus::GUI_WELCOME));

    // DFU mode blocks transitions
    status.currentGUI = CrosspadStatus::GUI_DFU;
    REQUIRE_FALSE(status.SetCurrentGUI(CrosspadStatus::GUI_MAIN));
}

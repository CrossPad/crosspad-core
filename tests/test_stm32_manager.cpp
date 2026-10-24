// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"
#include "crosspad/stm32/Stm32Manager.hpp"
#include "crosspad/stm32/Stm32ConfigSync.hpp"
#include "crosspad/stm32/reg_map.h"

using namespace crosspad;
using namespace test;

namespace {

class CountingSender : public Stm32CommandSender {
public:
    int keepAlives = 0;
    int routings = 0;
    int brightnesses = 0;

    bool sendPowerOff() override { return true; }
    bool sendBrightness(uint8_t) override { ++brightnesses; return true; }
    bool sendKeepAlive() override { ++keepAlives; return true; }
    bool sendDfuRequest(bool) override { return true; }
    bool sendRoutingMatrix(const STMRoutingSettings&) override { ++routings; return true; }
};

constexpr int64_t kUsPerMs = 1000;

} // namespace

TEST_CASE("Stm32Manager: start pushes routing and a keep-alive, tick keeps the cadence", "[stm][manager]") {
    CountingSender sender;
    MockClock clock;
    clock.set(1000 * kUsPerMs);
    Stm32Manager mgr;
    mgr.init(sender, CrosspadSettings::getInstance(), clock);

    mgr.start();
    REQUIRE(mgr.isRunning());
    REQUIRE(sender.keepAlives == 1);
    REQUIRE(sender.routings == 1);

    mgr.tick();                                   // 1 s since boot: nothing due
    REQUIRE(sender.keepAlives == 1);
    REQUIRE(sender.routings == 1);

    clock.set(3000 * kUsPerMs);
    mgr.tick();                                   // routing every 3 s
    REQUIRE(sender.routings == 2);
    REQUIRE(sender.keepAlives == 1);

    clock.set(5000 * kUsPerMs);
    mgr.tick();                                   // keep-alive every 5 s
    REQUIRE(sender.keepAlives == 2);
    REQUIRE(sender.routings == 2);

    clock.set(6000 * kUsPerMs);
    mgr.tick();
    REQUIRE(sender.routings == 3);

    clock.set(9999 * kUsPerMs);
    mgr.tick();
    REQUIRE(sender.keepAlives == 2);
    clock.set(10000 * kUsPerMs);
    mgr.tick();
    REQUIRE(sender.keepAlives == 3);
}

TEST_CASE("Stm32Manager: tick does nothing before start or after stop", "[stm][manager]") {
    CountingSender sender;
    MockClock clock;
    clock.set(60000 * kUsPerMs);
    Stm32Manager mgr;
    mgr.init(sender, CrosspadSettings::getInstance(), clock);

    mgr.tick();
    REQUIRE(sender.keepAlives == 0);
    REQUIRE(sender.routings == 0);

    mgr.start();
    mgr.stop();
    REQUIRE_FALSE(mgr.isRunning());
    const int keepAlives = sender.keepAlives;
    const int routings = sender.routings;
    clock.advance(60000 * kUsPerMs);
    mgr.tick();
    REQUIRE(sender.keepAlives == keepAlives);
    REQUIRE(sender.routings == routings);
}

TEST_CASE("Stm32Manager: a requested keep-alive goes out on the next tick", "[stm][manager]") {
    CountingSender sender;
    MockClock clock;
    clock.set(1000 * kUsPerMs);
    Stm32Manager mgr;
    mgr.init(sender, CrosspadSettings::getInstance(), clock);
    mgr.start();

    mgr.requestKeepAlive();
    mgr.tick();
    REQUIRE(sender.keepAlives == 2);
    mgr.tick();
    REQUIRE(sender.keepAlives == 2);
}

TEST_CASE("Stm32Manager: register writes go through the config sync", "[stm][manager]") {
    CountingSender sender;
    MockStm32Bus bus;
    Stm32ConfigSync sync(bus);
    MockClock clock;
    clock.set(1000 * kUsPerMs);
    Stm32Manager mgr;
    mgr.init(sender, CrosspadSettings::getInstance(), clock);
    mgr.setConfigSync(&sync);
    mgr.start();

    bus.writes.clear();
    mgr.notifyUserActivity();
    mgr.tick();
    REQUIRE(bus.lastByteAt(REG_CMD_USER_ACTIVITY) == 1);
    REQUIRE(bus.lastByteAt(REG_CTL_AUDIO_EN) == 0);   // first decision is always written, silence = off

    bus.writes.clear();
    clock.set(1100 * kUsPerMs);
    mgr.tick();
    REQUIRE(bus.lastByteAt(REG_CTL_AUDIO_EN) == -1);  // unchanged decision, no write
    REQUIRE(bus.lastByteAt(REG_CMD_USER_ACTIVITY) == -1);

    bus.readValues[REG_IDENT_BOOT_COUNT] = 9;          // the STM rebooted
    mgr.setBrightness(40);
    const int brightnesses = sender.brightnesses;
    clock.set(3000 * kUsPerMs);
    mgr.tick();                                       // boot count polled on the routing pass
    REQUIRE(bus.lastByteAt(REG_CTL_MIDI_CHAN) >= 0);  // full config pushed again
    REQUIRE(sender.brightnesses == brightnesses + 1); // and the user's brightness
}

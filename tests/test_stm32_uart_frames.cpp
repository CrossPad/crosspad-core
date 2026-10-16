#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>
#include "crosspad/pad/IPadLogicHandler.hpp"
#include "crosspad/protocol/Stm32MessageHandler.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/status/CrosspadStatus.hpp"

using namespace crosspad;

namespace {

// CMD_BUTTONS_DIGITAL as a UART-only STM sends it: a set bit is a pad NOT
// pressed (pads 0..6 in b3 bits 6..0, 7..13 in b4, 14/15 in b5 bits 6/5),
// b5 bit 0 the power button.
std::vector<uint8_t> digitalFrame(uint16_t pressed, bool power)
{
    uint8_t b3 = 0, b4 = 0, b5 = power ? 0x01 : 0x00;
    for (int i = 0; i < 7; i++)  if (!(pressed & (1u << i))) b3 |= (uint8_t)(1u << (6 - i));
    for (int i = 7; i < 14; i++) if (!(pressed & (1u << i))) b4 |= (uint8_t)(1u << (6 - (i - 7)));
    if (!(pressed & (1u << 14))) b5 |= 0x40;
    if (!(pressed & (1u << 15))) b5 |= 0x20;
    return {CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER, CROSSPAD_CMD_BUTTONS_DIGITAL,
            b3, b4, b5, CROSSPAD_SYSEX_END};
}

} // namespace

TEST_CASE("digital button frames report each pad edge to the platform", "[stm][buttons]") {
    PadManager pads;
    CrosspadStatus status;
    Stm32MessageHandler h;
    h.init(pads, status);
    std::vector<std::pair<uint8_t, bool>> edges;
    h.setPadEdgeCallback([&](uint8_t pad, bool pressed) { edges.emplace_back(pad, pressed); });

    auto send = [&](uint16_t pressed, bool power) {
        const auto f = digitalFrame(pressed, power);
        h.handleMessage(f.data(), (unsigned)f.size());
    };

    send(0, false);
    REQUIRE(edges.empty());

    send((1u << 0) | (1u << 14) | (1u << 15), false);
    REQUIRE(edges == std::vector<std::pair<uint8_t, bool>>{{0, true}, {14, true}, {15, true}});

    edges.clear();
    send((1u << 0) | (1u << 14) | (1u << 15), false);   // the STM's periodic resend
    REQUIRE(edges.empty());

    send(1u << 14, true);
    REQUIRE(edges == std::vector<std::pair<uint8_t, bool>>{{0, false}, {15, false}});
    REQUIRE(h.input.powerBtnPressed);
}

TEST_CASE("a version reply reaches the platform", "[stm][version]") {
    PadManager pads;
    CrosspadStatus status;
    Stm32MessageHandler h;
    h.init(pads, status);
    int pcb = -1, api = -1;
    h.setVersionReplyCallback([&](uint8_t p, uint8_t a) { pcb = p; api = a; });

    const uint8_t shortFrame[] = {CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER,
                                  CROSSPAD_CMD_VERSION_REPLY, 19, CROSSPAD_SYSEX_END};
    h.handleMessage(shortFrame, sizeof(shortFrame));
    REQUIRE(pcb == -1);

    const uint8_t frame[] = {CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER,
                             CROSSPAD_CMD_VERSION_REPLY, 19, 0x10, CROSSPAD_SYSEX_END};
    h.handleMessage(frame, sizeof(frame));
    REQUIRE(pcb == 19);
    REQUIRE(api == 0x10);
}

TEST_CASE("an older STM's version reply, the PCB in the command byte", "[stm][version]") {
    PadManager pads;
    CrosspadStatus status;
    Stm32MessageHandler h;
    h.init(pads, status);
    int pcb = -1, api = -1;
    h.setVersionReplyCallback([&](uint8_t p, uint8_t a) { pcb = p; api = a; });

    const uint8_t statusFrame[] = {CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER,
                                   CROSSPAD_CMD_STATUS_REGISTER, 0x01, 0x02, 0x00, CROSSPAD_SYSEX_END};
    h.handleMessage(statusFrame, sizeof(statusFrame));
    REQUIRE(pcb == -1);

    const uint8_t oldReply[] = {CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER, 19, 0x10,
                                CROSSPAD_SYSEX_END};
    h.handleMessage(oldReply, sizeof(oldReply));
    REQUIRE(pcb == 19);
    REQUIRE(api == 0x10);
}

TEST_CASE("a rev 1.x encoder count steps once per click, two counts each", "[stm][encoder]") {
    PadManager pads;
    CrosspadStatus status;
    Stm32MessageHandler h;
    h.init(pads, status);
    auto count = [&](uint8_t cnt) {
        const uint8_t f[] = {CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER,
                             CROSSPAD_CMD_ENCODER_COUNTER, cnt, CROSSPAD_SYSEX_END};
        h.handleMessage(f, sizeof(f));
    };

    // Measured on PCB 1.9: each click moves the count by 2, the count runs
    // 0..60. The first report is one count out of the detent at 57.
    count(56);
    count(55);                     // the first detent seen: no step yet
    count(54); count(53);
    REQUIRE(h.takeEncoderSteps() == -1);
    count(51); count(49);          // a fast turn: the even counts unreported
    REQUIRE(h.takeEncoderSteps() == -2);

    count(50); count(49);          // a wobble in the detent
    count(48); count(49);
    REQUIRE(h.takeEncoderSteps() == 0);

    count(55); count(59); count(60); count(0);   // up through the wrap: 60 -> 0 is one count
    REQUIRE(h.takeEncoderSteps() == 6);
    count(1);                      // even raw counts are detents now
    REQUIRE(h.takeEncoderSteps() == 0);
    count(2);
    REQUIRE(h.takeEncoderSteps() == 1);
    count(1); count(0); count(60);                // and back down through it
    REQUIRE(h.takeEncoderSteps() == -1);
    REQUIRE(h.encoderDetent() == 3);
}

namespace {

class TouchSpy : public IPadLogicHandler {
public:
    void onActivate(PadManager&) override {}
    void onDeactivate(PadManager&) override {}
    void onPadPress(PadManager&, uint8_t pad, uint8_t vel) override { log.emplace_back('P', pad, vel); }
    void onPadRelease(PadManager&, uint8_t pad) override { log.emplace_back('R', pad, 0); }
    void onPadPressure(PadManager&, uint8_t, uint8_t) override {}
    void onPadTouch(PadManager&, uint8_t pad, bool touching) override {
        log.emplace_back(touching ? 'T' : 'U', pad, 0);
    }
    std::vector<std::tuple<char, uint8_t, uint8_t>> log;
};

} // namespace

TEST_CASE("a pad-touch frame reaches the active pad logic before its hit", "[stm][touch]") {
    PadManager pads;
    CrosspadStatus status;
    Stm32MessageHandler h;
    h.init(pads, status);
    auto spy = std::make_shared<TouchSpy>();
    pads.registerPadLogic("Spy", spy);
    pads.setActivePadLogic("Spy");

    auto touch = [&](uint8_t pad, uint8_t on) {
        const uint8_t f[] = {CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER, CROSSPAD_CMD_PAD_TOUCH,
                             pad, on, CROSSPAD_SYSEX_END};
        h.handleMessage(f, sizeof(f));
    };
    touch(3, 1);
    pads.handlePadPress(3, 90);           // the note-on, as the MIDI parser hands it over
    touch(7, 1);
    touch(7, 0);                          // a touch that never became a hit
    touch(16, 1);                         // no such pad: dropped
    const uint8_t shortFrame[] = {CROSSPAD_SYSEX_START, CROSSPAD_SYSEX_MANUFACTURER, CROSSPAD_CMD_PAD_TOUCH,
                                  2, CROSSPAD_SYSEX_END};
    h.handleMessage(shortFrame, sizeof(shortFrame));

    using E = std::tuple<char, uint8_t, uint8_t>;
    REQUIRE(spy->log == std::vector<E>{E{'T', 3, 0}, E{'P', 3, 90}, E{'T', 7, 0}, E{'U', 7, 0}});

    spy->log.clear();
    pads.setInputSuppressed(true);        // the MODE map takes the pads: no touch either
    touch(4, 1);
    REQUIRE(spy->log.empty());
}

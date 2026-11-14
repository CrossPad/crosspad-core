// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.hpp"
#include "crosspad/midi/MidiInputHandler.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"

using namespace crosspad;
using Msg = test::RecordingMidiOutput::Msg;

namespace {

/// MidiInputHandler wired to a recording BLE output, Midi2BLE on.
struct BleForwardRig {
    test::CoreTestPlatform platform;
    test::RecordingMidiOutput ble;
    MidiInputHandler in;
    CrosspadSettings* settings = CrosspadSettings::getInstance();
    bool savedMidi2Ble = settings->esp32Routing.Midi2BLE;

    BleForwardRig() {
        REQUIRE(platform.init());
        settings->esp32Routing.Midi2BLE = true;
        in.init(platform.padManager, platform.eventBus, settings);
        in.setBleOutput(&ble);
    }
    ~BleForwardRig() { settings->esp32Routing.Midi2BLE = savedMidi2Ble; }
};

} // namespace

/* The channel a platform hands to handleSample* is 0-based and reaches the BLE
 * output as it is: a pad's note and its pressure, bend, CC and program must
 * share one channel on the peer. */
TEST_CASE("MidiInputHandler: audio-routed input reaches BLE on the channel given", "[midi]") {
    BleForwardRig rig;

    for (uint8_t ch : {uint8_t(0), uint8_t(9), uint8_t(15)}) {
        rig.ble.clear();
        rig.in.handleSampleNoteOn(ch, 60, 100);
        rig.in.handleSampleAftertouch(ch, 60, 70);
        rig.in.handleSampleControlChange(ch, 7, 90);
        rig.in.handleSamplePitchBend(ch, -1234);
        rig.in.handleSampleProgramChange(ch, 5);
        rig.in.handleSampleNoteOff(ch, 60, 0);

        REQUIRE(rig.ble.count() == 6);
        const Msg::Type want[] = {Msg::NoteOn, Msg::Aftertouch, Msg::CC,
                                  Msg::PitchBend, Msg::ProgramChange, Msg::NoteOff};
        for (size_t i = 0; i < 6; ++i) {
            CHECK(rig.ble.msgs[i].type == want[i]);
            CHECK(rig.ble.msgs[i].channel == ch);
        }
        CHECK(rig.ble.msgs[1].note == 60);
        CHECK(rig.ble.msgs[1].value == 70);
        CHECK(rig.ble.msgs[2].controller == 7);
        CHECK(rig.ble.msgs[2].value == 90);
        CHECK(rig.ble.msgs[3].pitchBend == -1234);
        CHECK(rig.ble.msgs[4].value == 5);
    }
}

TEST_CASE("MidiInputHandler: Midi2BLE off forwards nothing to BLE", "[midi]") {
    BleForwardRig rig;
    rig.settings->esp32Routing.Midi2BLE = false;

    rig.in.handleSampleNoteOn(0, 60, 100);
    rig.in.handleSampleAftertouch(0, 60, 70);
    rig.in.handleSampleControlChange(0, 7, 90);
    rig.in.handleSamplePitchBend(0, 100);
    rig.in.handleSampleProgramChange(0, 5);
    rig.in.handleSampleNoteOff(0, 60, 0);

    CHECK(rig.ble.count() == 0);
}

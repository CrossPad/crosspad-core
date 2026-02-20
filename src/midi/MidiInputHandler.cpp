#include "crosspad/midi/MidiInputHandler.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/event/IEventBus.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/midi/IMidiOutput.hpp"

namespace crosspad {

void MidiInputHandler::init(PadManager& padManager, IEventBus& eventBus,
                             CrosspadSettings* settings, IMidiOutput* forwardTarget) {
    padManager_    = &padManager;
    eventBus_      = &eventBus;
    settings_      = settings;
    forwardTarget_ = forwardTarget;
}

void MidiInputHandler::setForwardTarget(IMidiOutput* target) {
    forwardTarget_ = target;
}

// ── External MIDI input ─────────────────────────────────────────────────

void MidiInputHandler::handleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    // Update pad state and LEDs
    padManager_->handleMidiNoteOn(channel, note, velocity);

    // Post to EventBus with MidiInput source to prevent feedback loops
    eventBus_->postNoteOn(channel, note, velocity, EventSource::MidiInput);

    // Route to forwarding target if configured (e.g. BLE → STM32)
    if (forwardTarget_ && settings_ && settings_->esp32Routing.BLE2Midi) {
        forwardTarget_->sendNoteOn(note, velocity, channel);
    }
}

void MidiInputHandler::handleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    padManager_->handleMidiNoteOff(channel, note);

    eventBus_->postNoteOff(channel, note, velocity, EventSource::MidiInput);

    if (forwardTarget_ && settings_ && settings_->esp32Routing.BLE2Midi) {
        forwardTarget_->sendNoteOff(note, velocity, channel);
    }
}

void MidiInputHandler::handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) {
    if (forwardTarget_ && settings_ && settings_->esp32Routing.BLE2Midi) {
        forwardTarget_->sendControlChange(controller, value, channel);
    }
}

void MidiInputHandler::handleAftertouch(uint8_t channel, uint8_t note, uint8_t pressure) {
    padManager_->handleMidiAftertouch(channel, note, pressure);
}

// ── Audio-routed MIDI input ─────────────────────────────────────────────

void MidiInputHandler::handleSampleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    eventBus_->postNoteOn(channel, note, velocity, EventSource::MidiInput);
    padManager_->handleSamplePlay(note, true);
}

void MidiInputHandler::handleSampleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    eventBus_->postNoteOff(channel, note, velocity, EventSource::MidiInput);
    padManager_->handleSamplePlay(note, false);
}

} // namespace crosspad

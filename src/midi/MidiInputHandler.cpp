#include "crosspad/midi/MidiInputHandler.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/event/IEventBus.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"

namespace crosspad {

void MidiInputHandler::init(PadManager& padManager, IEventBus& eventBus,
                             CrosspadSettings* settings) {
    padManager_ = &padManager;
    eventBus_   = &eventBus;
    settings_   = settings;
}

void MidiInputHandler::setStm32Output(IMidiOutput* output) { stm32Output_ = output; }
void MidiInputHandler::setUsbOutput(IMidiOutput* output)   { usbOutput_ = output; }
void MidiInputHandler::setBleOutput(IMidiOutput* output)   { bleOutput_ = output; }

// ── External MIDI input (BLE / USB host) ─────────────────────────────────

void MidiInputHandler::handleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    padManager_->handleMidiNoteOn(channel, note, velocity);
    eventBus_->postNoteOn(channel, note, velocity, EventSource::MidiInput);

    if (settings_ && settings_->esp32Routing.BLE2Midi && stm32Output_)
        stm32Output_->sendNoteOn(note, velocity, channel);
}

void MidiInputHandler::handleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    padManager_->handleMidiNoteOff(channel, note);
    eventBus_->postNoteOff(channel, note, velocity, EventSource::MidiInput);

    if (settings_ && settings_->esp32Routing.BLE2Midi && stm32Output_)
        stm32Output_->sendNoteOff(note, velocity, channel);
}

void MidiInputHandler::handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) {
    if (settings_ && settings_->esp32Routing.BLE2Midi && stm32Output_)
        stm32Output_->sendControlChange(controller, value, channel);
}

void MidiInputHandler::handleAftertouch(uint8_t channel, uint8_t note, uint8_t pressure) {
    padManager_->handleMidiAftertouch(channel, note, pressure);
}

// ── Audio-routed MIDI input (STM32 UART / USB MIDI in) ──────────────────

void MidiInputHandler::handleSampleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    // Pad state/LED dispatch happens in the platform MIDI callbacks via
    // stm32NoteToPad() + PadManager::notifyNoteOn — getPadForMidiNote()
    // corrupts under sampler kit configs, so it must not be used here.
    eventBus_->postNoteOn(channel, note, velocity, EventSource::MidiInput);
    padManager_->handleSamplePlay(note, true);

    if (settings_ && settings_->esp32Routing.Midi2BLE && bleOutput_)
        bleOutput_->sendNoteOn(note, velocity, channel);
}

void MidiInputHandler::handleSampleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    // Symmetric with handleSampleNoteOn — mapping owned by platform callbacks.
    eventBus_->postNoteOff(channel, note, velocity, EventSource::MidiInput);
    padManager_->handleSamplePlay(note, false);

    if (settings_ && settings_->esp32Routing.Midi2BLE && bleOutput_)
        bleOutput_->sendNoteOff(note, velocity, channel);
}

void MidiInputHandler::handleSampleControlChange(uint8_t channel, uint8_t controller, uint8_t value) {
    if (settings_ && settings_->esp32Routing.Midi2BLE && bleOutput_)
        bleOutput_->sendControlChange(controller, value, channel);
}

void MidiInputHandler::handleSampleAftertouch(uint8_t channel, uint8_t note, uint8_t pressure) {
    // Mapping owned by platform callbacks (stm32NoteToPad + notifyPadPressure).
    if (settings_ && settings_->esp32Routing.Midi2BLE && bleOutput_)
        bleOutput_->sendAftertouch(note, pressure, channel);
}

void MidiInputHandler::handleSamplePitchBend(uint8_t channel, int16_t value) {
    if (settings_ && settings_->esp32Routing.Midi2BLE && bleOutput_)
        bleOutput_->sendPitchBend(value, channel);
}

void MidiInputHandler::handleSampleProgramChange(uint8_t channel, uint8_t program) {
    if (settings_ && settings_->esp32Routing.Midi2BLE && bleOutput_)
        bleOutput_->sendProgramChange(program, channel);
}

// ── IMidiOutput (pad output routing via keypad settings) ─────────────────

void MidiInputHandler::sendNoteOn(uint8_t note, uint8_t channel) {
    sendNoteOn(note, 127, channel);
}

void MidiInputHandler::sendNoteOff(uint8_t note, uint8_t channel) {
    sendNoteOff(note, 0, channel);
}

void MidiInputHandler::sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel) {
    if (!settings_) return;
    if (settings_->keypad.send2STM && stm32Output_)
        stm32Output_->sendNoteOn(note, velocity, channel);
    if (settings_->keypad.send2USB && usbOutput_)
        usbOutput_->sendNoteOn(note, velocity, channel);
    if (settings_->keypad.send2BLE && bleOutput_)
        bleOutput_->sendNoteOn(note, velocity, channel);
}

void MidiInputHandler::sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel) {
    if (!settings_) return;
    if (settings_->keypad.send2STM && stm32Output_)
        stm32Output_->sendNoteOff(note, velocity, channel);
    if (settings_->keypad.send2USB && usbOutput_)
        usbOutput_->sendNoteOff(note, velocity, channel);
    if (settings_->keypad.send2BLE && bleOutput_)
        bleOutput_->sendNoteOff(note, velocity, channel);
}

void MidiInputHandler::sendControlChange(uint8_t controller, uint8_t value, uint8_t channel) {
    if (!settings_) return;
    if (settings_->keypad.send2STM && stm32Output_)
        stm32Output_->sendControlChange(controller, value, channel);
    if (settings_->keypad.send2USB && usbOutput_)
        usbOutput_->sendControlChange(controller, value, channel);
    if (settings_->keypad.send2BLE && bleOutput_)
        bleOutput_->sendControlChange(controller, value, channel);
}

void MidiInputHandler::sendAftertouch(uint8_t note, uint8_t pressure, uint8_t channel) {
    if (!settings_) return;
    if (settings_->keypad.send2STM && stm32Output_)
        stm32Output_->sendAftertouch(note, pressure, channel);
    if (settings_->keypad.send2USB && usbOutput_)
        usbOutput_->sendAftertouch(note, pressure, channel);
    if (settings_->keypad.send2BLE && bleOutput_)
        bleOutput_->sendAftertouch(note, pressure, channel);
}

void MidiInputHandler::sendProgramChange(uint8_t program, uint8_t channel) {
    if (!settings_) return;
    if (settings_->keypad.send2STM && stm32Output_)
        stm32Output_->sendProgramChange(program, channel);
    if (settings_->keypad.send2USB && usbOutput_)
        usbOutput_->sendProgramChange(program, channel);
    if (settings_->keypad.send2BLE && bleOutput_)
        bleOutput_->sendProgramChange(program, channel);
}

void MidiInputHandler::sendPitchBend(int16_t value, uint8_t channel) {
    if (!settings_) return;
    if (settings_->keypad.send2STM && stm32Output_)
        stm32Output_->sendPitchBend(value, channel);
    if (settings_->keypad.send2USB && usbOutput_)
        usbOutput_->sendPitchBend(value, channel);
    if (settings_->keypad.send2BLE && bleOutput_)
        bleOutput_->sendPitchBend(value, channel);
}

} // namespace crosspad

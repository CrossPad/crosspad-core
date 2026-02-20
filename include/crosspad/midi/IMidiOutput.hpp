#pragma once
#include <cstdint>

namespace crosspad {

/**
 * @brief Abstract MIDI output interface
 *
 * Platform implementations inject callbacks via EspMidiOutput::setCallbacks()
 * or provide a concrete subclass (ArduinoMidiOutput, etc.).
 *
 * The 2-param sendNoteOn/Off are the required pure-virtual methods (legacy,
 * velocity-less). The 3-param velocity-aware overloads and the additional
 * message types have default implementations so existing subclasses don't break.
 */
class IMidiOutput {
public:
    virtual ~IMidiOutput() = default;

    // ── Required (pure virtual) ─────────────────────────────────────────
    virtual void sendNoteOn(uint8_t note, uint8_t channel) = 0;
    virtual void sendNoteOff(uint8_t note, uint8_t channel) = 0;

    // ── Velocity-aware overloads (default delegates to legacy version) ──
    virtual void sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel) {
        (void)velocity;
        sendNoteOn(note, channel);
    }
    virtual void sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel) {
        (void)velocity;
        sendNoteOff(note, channel);
    }

    // ── Additional message types (no-op defaults) ───────────────────────
    virtual void sendControlChange(uint8_t controller, uint8_t value, uint8_t channel) {
        (void)controller; (void)value; (void)channel;
    }
    virtual void sendAftertouch(uint8_t note, uint8_t pressure, uint8_t channel) {
        (void)note; (void)pressure; (void)channel;
    }
    virtual void sendProgramChange(uint8_t program, uint8_t channel) {
        (void)program; (void)channel;
    }
};

/// Platform-injected singleton (same pattern as getEventBus())
IMidiOutput& getMidiOutput();

} // namespace crosspad

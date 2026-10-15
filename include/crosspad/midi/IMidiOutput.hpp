// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace crosspad {

/**
 * @brief Abstract MIDI output interface
 *
 * On CrossPad the output an app gets from getMidiOutput() sends each message
 * to the MIDI destinations enabled in the pad settings (the STM32 link, USB
 * MIDI, Bluetooth MIDI). A platform provides a subclass per transport.
 *
 * Channels are 0-based (0..15 for MIDI channels 1..16). Notes, velocities
 * and controller values are 7-bit (0..127).
 *
 * A subclass must implement the two-argument sendNoteOn()/sendNoteOff();
 * every other method has a default, so a subclass overrides only the
 * messages its transport carries.
 */
class IMidiOutput {
public:
    virtual ~IMidiOutput() = default;

    // ── Required (pure virtual) ─────────────────────────────────────────
    /// Send a Note On at full velocity (127).
    virtual void sendNoteOn(uint8_t note, uint8_t channel) = 0;
    /// Send a Note Off with release velocity 0.
    virtual void sendNoteOff(uint8_t note, uint8_t channel) = 0;

    // ── With velocity (default: the two-argument form, velocity dropped) ──
    /// Send a Note On with @p velocity (1..127).
    virtual void sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel) {
        (void)velocity;
        sendNoteOn(note, channel);
    }
    /// Send a Note Off with release @p velocity.
    virtual void sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel) {
        (void)velocity;
        sendNoteOff(note, channel);
    }

    // ── Other channel messages (default: not sent) ──────────────────────
    /// Send a Control Change: @p controller number and @p value, both 0..127.
    virtual void sendControlChange(uint8_t controller, uint8_t value, uint8_t channel) {
        (void)controller; (void)value; (void)channel;
    }
    /// Send polyphonic aftertouch: @p pressure (0..127) on the held @p note.
    virtual void sendAftertouch(uint8_t note, uint8_t pressure, uint8_t channel) {
        (void)note; (void)pressure; (void)channel;
    }
    /// Send a Program Change to @p program (0..127).
    virtual void sendProgramChange(uint8_t program, uint8_t channel) {
        (void)program; (void)channel;
    }
    /// Send a Pitch Bend: @p value -8192 (down) .. 8191 (up), 0 centred.
    virtual void sendPitchBend(int16_t value, uint8_t channel) {
        (void)value; (void)channel;
    }
    /// Whole frame including F0 and F7. Default drops it: only the USB
    /// output talks to a host that speaks the CrossPad SysEx catalog.
    virtual void sendSysEx(const uint8_t* frame, size_t len) {
        (void)frame; (void)len;
    }
};

/// The platform's MIDI output; never null (it sends nothing on a platform
/// without MIDI).
IMidiOutput& getMidiOutput();

} // namespace crosspad

#pragma once

#include <cstdint>

namespace crosspad {

/**
 * @brief Result of parsing a single CLI character for sequencer control.
 *
 * Pure data — no side effects. The platform layer interprets the result
 * and performs I/O (console output, note triggering, etc.).
 */
struct SequencerCliResult {
    enum Action {
        None,
        Record,
        Playback,
        Overdub,
        Stop,
        Reset,
        ToggleClick,
        SetLength,      ///< length field contains the value (1-4)
        BpmUp,
        BpmDown,
        ToggleLive,
        NoteOn,         ///< note field contains MIDI note, velocity = 127
        ShowHelp,
    };

    Action  action   = None;
    uint8_t note     = 0;
    uint8_t velocity = 127;
    uint8_t length   = 0;
};

/**
 * @brief Parse a single character into a sequencer CLI action.
 *
 * Pure function with no side effects. Maps keyboard characters to
 * sequencer operations.
 *
 * @param c  Input character from serial/stdin
 * @return   Parsed action (None if character is not recognized)
 */
inline SequencerCliResult parseSequencerCliChar(char c) {
    SequencerCliResult r;

    switch (c) {
    case '+': r.action = SequencerCliResult::BpmUp;       break;
    case '-': r.action = SequencerCliResult::BpmDown;     break;
    case 'r': r.action = SequencerCliResult::Record;      break;
    case 'p': r.action = SequencerCliResult::Playback;    break;
    case 'd': r.action = SequencerCliResult::Overdub;     break;
    case 's': r.action = SequencerCliResult::Stop;        break;
    case 'c': r.action = SequencerCliResult::ToggleClick; break;
    case 'q': r.action = SequencerCliResult::Reset;       break;
    case 'l': r.action = SequencerCliResult::ToggleLive;  break;
    case '?': r.action = SequencerCliResult::ShowHelp;    break;

    case '1': r.action = SequencerCliResult::SetLength; r.length = 1; break;
    case '2': r.action = SequencerCliResult::SetLength; r.length = 2; break;
    case '3': r.action = SequencerCliResult::SetLength; r.length = 3; break;
    case '4': r.action = SequencerCliResult::SetLength; r.length = 4; break;

    // Pad note triggers (MIDI notes 12-27)
    case 'b': r.action = SequencerCliResult::NoteOn; r.note = 12; break;
    case 'n': r.action = SequencerCliResult::NoteOn; r.note = 13; break;
    case 'm': r.action = SequencerCliResult::NoteOn; r.note = 14; break;
    case ',': r.action = SequencerCliResult::NoteOn; r.note = 15; break;
    case 'g': r.action = SequencerCliResult::NoteOn; r.note = 16; break;
    case 'h': r.action = SequencerCliResult::NoteOn; r.note = 17; break;
    case 'j': r.action = SequencerCliResult::NoteOn; r.note = 18; break;
    case 'k': r.action = SequencerCliResult::NoteOn; r.note = 19; break;
    case 't': r.action = SequencerCliResult::NoteOn; r.note = 20; break;
    case 'z': r.action = SequencerCliResult::NoteOn; r.note = 21; break;
    case 'u': r.action = SequencerCliResult::NoteOn; r.note = 22; break;
    case 'i': r.action = SequencerCliResult::NoteOn; r.note = 23; break;
    case '5': r.action = SequencerCliResult::NoteOn; r.note = 24; break;
    case '6': r.action = SequencerCliResult::NoteOn; r.note = 25; break;
    case '7': r.action = SequencerCliResult::NoteOn; r.note = 26; break;
    case '8': r.action = SequencerCliResult::NoteOn; r.note = 27; break;

    default: break;
    }

    return r;
}

} // namespace crosspad

// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file EventTypes.hpp
 * @brief Portable event type and source enumerations for CrossPad event system
 *
 * Pure C++ — no platform dependencies. Used by IEventBus and all subscribers.
 */
#pragma once

#include <cstdint>

namespace crosspad {

/**
 * @brief Event types dispatched through the CrossPad event bus
 */
enum class EventType : int32_t {
    NoteOn,             ///< a note started (NoteOnData)
    NoteOff,            ///< a note ended (NoteOffData)

    PadPressed,         ///< a pad was pressed (PadPressedData)
    PadReleased,        ///< a pad was released (PadReleasedData)

    AppChanged,         ///< an app announced a change of its own (AppChangedData); see IEventBus::postAppChanged()
    TempoChanged,       ///< the tempo changed (TempoChangedData)
    LiveModeChanged,    ///< live mode was switched (LiveModeChangedData)

    GuiUpdateRequired,  ///< the GUI should refresh; the data is the EventSource

    SamplePlayback,     ///< a sample started or stopped on a pad (SamplePlaybackData)

    MotionGesture,      ///< board motion recognised by the IMU: tap, double tap, free fall, ... (MotionGestureData)

    Max                 ///< number of event types; not an event
};

/**
 * @brief Event source identifiers for filtering in subscribers
 *
 * Subscribers can check the source to decide whether to handle an event --
 * for example, to tell a finger's press (PadManager) from the loop
 * recorder playing a take back (SequencerCallback).
 */
enum class EventSource : uint8_t {
    PadManager,         ///< Physical pad presses
    MidiInput,          ///< External MIDI devices (USB, UART, BLE)
    Cli,                ///< Command line interface / serial console
    App,                ///< Generic application source
    AppOther,           ///< Events from other apps (cross-app communication)
    System,             ///< System-wide events (init, shutdown)
    Gui,                ///< GUI-related events (screen changes, user interaction)
    SequencerCallback,  ///< Sequencer playback triggering samples
    SequencerInput,     ///< Live pad input during sequencer recording
    SamplePlayback,     ///< Sample playback state info (wavIdx, note)
    Imu,                ///< Motion recognised in the IMU itself

    Max                 ///< number of sources; not a source
};

/**
 * @brief Get human-readable name for an event source (for logging)
 */
inline const char* getSourceName(EventSource source) {
    switch (source) {
    case EventSource::PadManager:        return "PAD_MANAGER";
    case EventSource::MidiInput:         return "MIDI_INPUT";
    case EventSource::Cli:               return "CLI";
    case EventSource::App:               return "APP";
    case EventSource::AppOther:          return "APP_OTHER";
    case EventSource::System:            return "SYSTEM";
    case EventSource::Gui:               return "GUI";
    case EventSource::SequencerCallback: return "SEQUENCER_CB";
    case EventSource::SequencerInput:    return "SEQUENCER_INPUT";
    case EventSource::SamplePlayback:    return "SAMPLE_PLAYBACK";
    case EventSource::Imu:               return "IMU";
    default:                             return "UNKNOWN";
    }
}

/**
 * @brief Get human-readable name for an event type (for logging)
 */
inline const char* getEventName(EventType type) {
    switch (type) {
    case EventType::NoteOn:            return "NOTE_ON";
    case EventType::NoteOff:           return "NOTE_OFF";
    case EventType::PadPressed:        return "PAD_PRESSED";
    case EventType::PadReleased:       return "PAD_RELEASED";
    case EventType::AppChanged:        return "APP_CHANGED";
    case EventType::TempoChanged:      return "TEMPO_CHANGED";
    case EventType::LiveModeChanged:   return "LIVE_MODE_CHANGED";
    case EventType::GuiUpdateRequired: return "GUI_UPDATE";
    case EventType::SamplePlayback:    return "SAMPLE_PLAYBACK";
    case EventType::MotionGesture:     return "MOTION_GESTURE";
    default:                           return "UNKNOWN";
    }
}

} // namespace crosspad

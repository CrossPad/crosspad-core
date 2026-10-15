// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file EventData.hpp
 * @brief Portable event data structures for CrossPad event system
 *
 * Pure C++ — no platform dependencies. Each struct corresponds to an EventType
 * and is passed as the event payload through IEventBus.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include "crosspad/event/EventTypes.hpp"

namespace crosspad {

/// Payload of EventType::NoteOn.
struct NoteOnData {
    uint8_t channel;      ///< MIDI channel, 0-based
    uint8_t note;         ///< MIDI note, 0-127
    uint8_t velocity;     ///< 0-127
    EventSource source;   ///< who played it
};

/// Payload of EventType::NoteOff.
struct NoteOffData {
    uint8_t channel;      ///< MIDI channel, 0-based
    uint8_t note;         ///< MIDI note, 0-127
    uint8_t velocity;     ///< release velocity, 0-127
    EventSource source;   ///< who played it
};

/// Payload of EventType::PadPressed.
struct PadPressedData {
    uint8_t padIdx;       ///< 0-15
    bool isOn;            ///< true for a press
    uint8_t velocity;     ///< 0-127
    EventSource source;   ///< who pressed
};

/// Payload of EventType::PadReleased.
struct PadReleasedData {
    uint8_t padIdx;       ///< 0-15
    EventSource source;   ///< who released
};

/// Payload of EventType::AppChanged: the app that announced a change
/// (IEventBus::postAppChanged()).
struct AppChangedData {
    char appName[32];     ///< the app's name, NUL-terminated
    EventSource source;

    /// Copy @p name into appName, cut to 31 characters.
    void setName(const char* name) {
        strncpy(appName, name, sizeof(appName) - 1);
        appName[sizeof(appName) - 1] = '\0';
    }
};

/// Payload of EventType::TempoChanged.
struct TempoChangedData {
    float bpm;            ///< the new tempo, beats per minute
    EventSource source;
};

/// Payload of EventType::SamplePlayback.
struct SamplePlaybackData {
    uint8_t note;       ///< Pad index (0-15), or 16+ for preview channel
    uint8_t wavIdx;     ///< Actual sample index being played
    bool isOn;          ///< true = note on, false = note off
    uint8_t velocity;   ///< 1-127 playback velocity (ignored when isOn = false)
    EventSource source;
    /// Semitones on top of the pad's own pitch, for this note only: 16 levels
    /// of pitch play one pad's sample from every pad.
    int8_t transpose = 0;
    /// Which hit this is, for the release and the per-pad retrigger, when it
    /// is not the pad itself: two pads playing one sample at two pitches are
    /// two notes, not one note cut short. kSamplePlaybackOwnKey: the pad.
    uint8_t key = 0xFF;
};
/// SamplePlaybackData::key of a plain pad hit: the pad itself.
constexpr uint8_t kSamplePlaybackOwnKey = 0xFF;

/// Payload of EventType::LiveModeChanged.
struct LiveModeChangedData {
    bool enabled;         ///< live mode on
    EventSource source;
};

/// What the motion sensor recognised. The names are the gestures, not a
/// particular part's register bits — a platform whose IMU knows fewer of them
/// simply never posts the rest.
enum class MotionGesture : uint8_t {
    SingleTap,      ///< one tap on the board
    DoubleTap,      ///< two taps in quick succession
    FreeFall,       ///< the board is falling
    WakeUp,         ///< motion after a still period
    Orientation,    ///< the board was turned to another face
    Still,          ///< no motion long enough for the sensor to say so
};

/// Payload of EventType::MotionGesture.
struct MotionGestureData {
    MotionGesture gesture;   ///< what was recognised
    uint8_t orientation;   ///< face the board rests on, meaningful for Orientation
    EventSource source;
};

} // namespace crosspad

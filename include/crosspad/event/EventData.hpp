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

struct NoteOnData {
    uint8_t channel;
    uint8_t note;
    uint8_t velocity;
    EventSource source;
};

struct NoteOffData {
    uint8_t channel;
    uint8_t note;
    uint8_t velocity;
    EventSource source;
};

struct PadPressedData {
    uint8_t padIdx;
    bool isOn;
    uint8_t velocity;
    EventSource source;
};

struct PadReleasedData {
    uint8_t padIdx;
    EventSource source;
};

struct AppChangedData {
    char appName[32];
    EventSource source;

    void setName(const char* name) {
        strncpy(appName, name, sizeof(appName) - 1);
        appName[sizeof(appName) - 1] = '\0';
    }
};

struct TempoChangedData {
    float bpm;
    EventSource source;
};

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
constexpr uint8_t kSamplePlaybackOwnKey = 0xFF;

struct LiveModeChangedData {
    bool enabled;
    EventSource source;
};

/// What the motion sensor recognised. The names are the gestures, not a
/// particular part's register bits — a platform whose IMU knows fewer of them
/// simply never posts the rest.
enum class MotionGesture : uint8_t {
    SingleTap,
    DoubleTap,
    FreeFall,
    WakeUp,         ///< motion after a still period
    Orientation,    ///< the board was turned to another face
    Still,          ///< no motion long enough for the sensor to say so
};

struct MotionGestureData {
    MotionGesture gesture;
    uint8_t orientation;   ///< face the board rests on, meaningful for Orientation
    EventSource source;
};

} // namespace crosspad

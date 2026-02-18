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
    EventSource source;
};

struct LiveModeChangedData {
    bool enabled;
    EventSource source;
};

} // namespace crosspad

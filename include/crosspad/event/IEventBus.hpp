// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file IEventBus.hpp
 * @brief Platform-agnostic event bus interface for CrossPad
 *
 * Pure C++ -- no platform dependencies. Platform implementations (ESP-IDF,
 * Linux, etc.) implement this interface to provide thread-safe event dispatch.
 *
 * - Handlers have the signature handler(event_data_ptr, event_type,
 *   user_context); the data is valid only during the call.
 * - post methods are thread-safe from any context. On the firmware and the
 *   simulator they queue the event and a dispatch task calls the handlers;
 *   an implementation may also dispatch at once on the calling thread
 *   (SyncEventBus, used by the host tests). Write handlers that work either
 *   way.
 * - send methods dispatch at once, for time-critical paths.
 * - subscribe() returns an opaque handle that unsubscribe() takes.
 *
 * Usage:
 * ```cpp
 * auto& bus = platform.getEventBus();
 *
 * auto handle = bus.subscribe(EventType::NoteOn,
 *     [](const void* data, EventType type, void* ctx) {
 *         auto* note = static_cast<const NoteOnData*>(data);
 *         // handle note on
 *     }, this);
 *
 * bus.postNoteOn(0, 60, 127, EventSource::PadManager);
 *
 * bus.unsubscribe(handle);
 * ```
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include "crosspad/event/EventTypes.hpp"
#include "crosspad/event/EventData.hpp"

namespace crosspad {

/**
 * @brief Event handler callback signature
 *
 * @param event_data  Pointer to event-specific data struct (NoteOnData, etc.)
 *                    Valid only for the duration of the callback.
 * @param event_type  Which event type triggered this callback
 * @param user_ctx    User context pointer passed during subscribe()
 */
using EventHandler = void (*)(const void* event_data, EventType event_type, void* user_ctx);

/**
 * @brief Opaque handle returned by subscribe(), used for unsubscribe()
 */
using SubscriptionHandle = void*;

/**
 * @brief Abstract event bus interface
 *
 * Thread-safe event dispatch. Platform implementations provide the actual
 * queue and dispatch mechanism (esp_event on the firmware, direct dispatch
 * in SyncEventBus). Every post method returns false when the event could
 * not be posted.
 */
class IEventBus {
public:
    virtual ~IEventBus() = default;

    /// Initialize the event bus (create queue, start dispatch task if needed).
    /// The platform calls it once at start-up.
    virtual bool init() = 0;

    // ── Subscription ────────────────────────────────────────────────────

    /// Subscribe to a specific event type. Returns handle for unsubscribe.
    virtual SubscriptionHandle subscribe(EventType type, EventHandler handler, void* user_ctx = nullptr) = 0;

    /// Unsubscribe using the handle returned by subscribe().
    ///
    /// After unsubscribe() returns, the handler is not running and will not
    /// be called again -- except when unsubscribe() is called from inside a
    /// handler on the dispatch thread: then the call in progress completes
    /// and no further calls happen. A handler subscribed while a dispatch is
    /// in progress is first called by the next dispatch.
    ///
    /// Waiting for a running handler is why a handler must never block on
    /// anything the unsubscribing thread may hold -- the LVGL lock on the UI
    /// thread, where apps unsubscribe in their teardown.
    virtual void unsubscribe(SubscriptionHandle handle) = 0;

    // ── Async posting (queued, thread-safe from any context) ────────────

    /// Post EventType::NoteOn (NoteOnData).
    virtual bool postNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) = 0;
    /// Post EventType::NoteOff (NoteOffData).
    virtual bool postNoteOff(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) = 0;
    /// Post EventType::PadPressed (PadPressedData).
    virtual bool postPadPressed(uint8_t padIdx, bool isOn, uint8_t velocity, EventSource source = EventSource::PadManager) = 0;
    /// Post EventType::PadReleased (PadReleasedData).
    virtual bool postPadReleased(uint8_t padIdx, EventSource source = EventSource::PadManager) = 0;
    /// Post EventType::AppChanged (AppChangedData) with @p appName. An app
    /// posts it to announce a change of its own -- the Sampler does after it
    /// changes the kit; opening and closing apps does not post it.
    virtual bool postAppChanged(const char* appName, EventSource source = EventSource::System) = 0;
    /// Post EventType::TempoChanged (TempoChangedData), @p bpm in beats per
    /// minute.
    virtual bool postTempoChanged(float bpm, EventSource source = EventSource::SequencerCallback) = 0;
    /// Post EventType::LiveModeChanged (LiveModeChangedData).
    virtual bool postLiveModeChanged(bool enabled, EventSource source = EventSource::System) = 0;
    /// Post EventType::GuiUpdateRequired; the data is the EventSource.
    virtual bool postGuiUpdateRequired(EventSource source = EventSource::Gui) = 0;
    /// Post EventType::SamplePlayback (SamplePlaybackData): a sample started
    /// (@p isOn) or stopped on pad @p note.
    ///
    /// @param velocity 1-127 playback velocity; defaults to full scale so
    ///        callers that have no velocity (preview, kit stop-all) are
    ///        unaffected. Ignored when isOn = false.
    /// @param transpose, key  see SamplePlaybackData; a plain pad hit leaves
    ///        them at 0 and the pad's own key.
    virtual bool postSamplePlayback(uint8_t note, uint8_t wavIdx, bool isOn, EventSource source = EventSource::SamplePlayback, uint8_t velocity = 127,
                                    int8_t transpose = 0, uint8_t key = kSamplePlaybackOwnKey) = 0;

    /// Board motion the sensor itself recognised. Apps subscribe to
    /// EventType::MotionGesture instead of talking to any IMU driver.
    virtual bool postMotionGesture(MotionGesture gesture, uint8_t orientation = 0, EventSource source = EventSource::Imu) = 0;

    // ── Sync posting (immediate dispatch, for time-critical paths) ──────

    /// Dispatch EventType::NoteOn (NoteOnData) at once, on the calling thread.
    virtual bool sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source = EventSource::System) = 0;
    /// Dispatch EventType::NoteOff (NoteOffData) at once, on the calling thread.
    virtual bool sendNoteOff(uint8_t channel, uint8_t note, EventSource source = EventSource::System) = 0;
};

/// The platform's event bus.
IEventBus& getEventBus();

} // namespace crosspad

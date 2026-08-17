/**
 * @file IEventBus.hpp
 * @brief Platform-agnostic event bus interface for CrossPad
 *
 * Pure C++ — no platform dependencies. Platform implementations (ESP-IDF,
 * Linux, etc.) implement this interface to provide thread-safe event dispatch.
 *
 * Design decisions:
 * - Virtual interface (not #ifdef) for testability and clean separation
 * - Callback signature mirrors esp_event_handler_t semantics but without
 *   platform types: handler(event_data_ptr, event_type, user_context)
 * - All post methods are asynchronous (queued) — thread-safe from any context
 * - send methods are synchronous — for ISR or time-critical paths
 * - Subscription uses opaque handle for unsubscribe (not raw function pointer)
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
 * Thread-safe, asynchronous event dispatch. Platform implementations provide
 * the actual queue and dispatch mechanism (esp_event, std::queue, etc.)
 */
class IEventBus {
public:
    virtual ~IEventBus() = default;

    /// Initialize the event bus (create queue, start dispatch task if needed)
    virtual bool init() = 0;

    // ── Subscription ────────────────────────────────────────────────────

    /// Subscribe to a specific event type. Returns handle for unsubscribe.
    virtual SubscriptionHandle subscribe(EventType type, EventHandler handler, void* user_ctx = nullptr) = 0;

    /// Unsubscribe using the handle returned by subscribe()
    virtual void unsubscribe(SubscriptionHandle handle) = 0;

    // ── Async posting (queued, thread-safe from any context) ────────────

    virtual bool postNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) = 0;
    virtual bool postNoteOff(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) = 0;
    virtual bool postPadPressed(uint8_t padIdx, bool isOn, uint8_t velocity, EventSource source = EventSource::PadManager) = 0;
    virtual bool postPadReleased(uint8_t padIdx, EventSource source = EventSource::PadManager) = 0;
    virtual bool postAppChanged(const char* appName, EventSource source = EventSource::System) = 0;
    virtual bool postTempoChanged(float bpm, EventSource source = EventSource::SequencerCallback) = 0;
    virtual bool postLiveModeChanged(bool enabled, EventSource source = EventSource::System) = 0;
    virtual bool postGuiUpdateRequired(EventSource source = EventSource::Gui) = 0;
    /// @param velocity 1-127 playback velocity; defaults to full scale so
    ///        callers that have no velocity (preview, kit stop-all) are
    ///        unaffected. Ignored when isOn = false.
    virtual bool postSamplePlayback(uint8_t note, uint8_t wavIdx, bool isOn, EventSource source = EventSource::SamplePlayback, uint8_t velocity = 127) = 0;

    // ── Sync posting (immediate dispatch, use from ISR or time-critical) ─

    virtual bool sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source = EventSource::System) = 0;
    virtual bool sendNoteOff(uint8_t channel, uint8_t note, EventSource source = EventSource::System) = 0;
};

/// Platform-injected singleton (implemented by PlatformServices)
IEventBus& getEventBus();

} // namespace crosspad

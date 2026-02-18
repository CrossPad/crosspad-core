/**
 * @file EspEventBus.hpp
 * @brief ESP-IDF implementation of IEventBus using esp_event system
 *
 * Wraps ESP-IDF's esp_event_loop for thread-safe, queue-based event dispatch.
 * Works on both ESP-IDF (native) and Arduino-ESP32 (which includes esp_event).
 */
#pragma once

#include "crosspad/event/IEventBus.hpp"
#include "esp_event.h"

namespace crosspad {

/// Configuration for the ESP-IDF event bus
struct EspEventBusConfig {
    size_t queueSize = 64;
    const char* taskName = "crosspad_events";
    uint8_t taskPriority = 4;
    size_t taskStackSize = 2048;
    int taskCoreId = 1;
};

/**
 * @brief ESP-IDF implementation of IEventBus
 *
 * Uses esp_event_loop_create() for a dedicated event loop with its own
 * FreeRTOS task. Events are queued via esp_event_post_to() (thread-safe)
 * and dispatched on the event loop task.
 *
 * Internal adapter: The crosspad::EventHandler signature is adapted to
 * esp_event_handler_t via a static trampoline function. Each subscription
 * stores the user's EventHandler + context in a small heap-allocated struct
 * that serves as both the trampoline's arg and the SubscriptionHandle.
 */
class EspEventBus : public IEventBus {
public:
    explicit EspEventBus(const EspEventBusConfig& config = {});
    ~EspEventBus() override;

    // Non-copyable
    EspEventBus(const EspEventBus&) = delete;
    EspEventBus& operator=(const EspEventBus&) = delete;

    bool init() override;

    SubscriptionHandle subscribe(EventType type, EventHandler handler, void* user_ctx = nullptr) override;
    void unsubscribe(SubscriptionHandle handle) override;

    bool postNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) override;
    bool postNoteOff(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) override;
    bool postPadPressed(uint8_t padIdx, bool isOn, uint8_t velocity, EventSource source) override;
    bool postPadReleased(uint8_t padIdx, EventSource source) override;
    bool postAppChanged(const char* appName, EventSource source) override;
    bool postTempoChanged(float bpm, EventSource source) override;
    bool postLiveModeChanged(bool enabled, EventSource source) override;
    bool postGuiUpdateRequired(EventSource source) override;
    bool postSamplePlayback(uint8_t note, uint8_t wavIdx, bool isOn, EventSource source) override;

    bool sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) override;
    bool sendNoteOff(uint8_t channel, uint8_t note, EventSource source) override;

private:
    /// Internal subscription record — heap allocated, serves as SubscriptionHandle
    struct Subscription {
        EventHandler handler;
        void* userCtx;
        EventType type;
        esp_event_handler_instance_t espInstance;
        EspEventBus* bus;  // back-pointer for trampoline
    };

    /// Trampoline: adapts esp_event_handler_t → crosspad::EventHandler
    static void espEventTrampoline(void* handler_arg, esp_event_base_t base,
                                   int32_t id, void* event_data);

    /// Helper: post raw event data
    bool postEvent(EventType type, const void* data, size_t dataSize);
    bool sendEvent(EventType type, const void* data, size_t dataSize);

    esp_event_loop_handle_t eventLoop_ = nullptr;
    bool initialized_ = false;
    EspEventBusConfig config_;
};

} // namespace crosspad

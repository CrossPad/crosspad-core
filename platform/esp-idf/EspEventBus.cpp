/**
 * @file EspEventBus.cpp
 * @brief ESP-IDF implementation of IEventBus
 */

#include "EspEventBus.hpp"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "EspEventBus";

// Event base for ESP-IDF event system
ESP_EVENT_DEFINE_BASE(CROSSPAD_EVENTS);

namespace crosspad {

EspEventBus::EspEventBus(const EspEventBusConfig& config)
    : config_(config)
{
}

EspEventBus::~EspEventBus() {
    if (eventLoop_) {
        esp_event_loop_delete(eventLoop_);
        eventLoop_ = nullptr;
    }
}

bool EspEventBus::init() {
    if (initialized_) return true;

    esp_event_loop_args_t loop_args = {
        .queue_size = static_cast<int32_t>(config_.queueSize),
        .task_name = config_.taskName,
        .task_priority = config_.taskPriority,
        .task_stack_size = config_.taskStackSize,
        .task_core_id = config_.taskCoreId
    };

    esp_err_t err = esp_event_loop_create(&loop_args, &eventLoop_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(err));
        return false;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "Initialized (queue=%d, core=%d, prio=%d)",
             config_.queueSize, config_.taskCoreId, config_.taskPriority);
    return true;
}

// ── Trampoline ──────────────────────────────────────────────────────────

void EspEventBus::espEventTrampoline(void* handler_arg, esp_event_base_t /*base*/,
                                     int32_t id, void* event_data) {
    auto* sub = static_cast<Subscription*>(handler_arg);
    if (sub && sub->handler) {
        sub->handler(event_data, static_cast<EventType>(id), sub->userCtx);
    }
}

// ── Subscribe / Unsubscribe ─────────────────────────────────────────────

SubscriptionHandle EspEventBus::subscribe(EventType type, EventHandler handler, void* user_ctx) {
    if (!initialized_) {
        if (!init()) return nullptr;
    }

    auto* sub = new Subscription{
        .handler = handler,
        .userCtx = user_ctx,
        .type = type,
        .espInstance = nullptr,
        .bus = this
    };

    esp_err_t err = esp_event_handler_instance_register_with(
        eventLoop_, CROSSPAD_EVENTS, static_cast<int32_t>(type),
        &EspEventBus::espEventTrampoline, sub, &sub->espInstance);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to %s: %s",
                 getEventName(type), esp_err_to_name(err));
        delete sub;
        return nullptr;
    }

    return static_cast<SubscriptionHandle>(sub);
}

void EspEventBus::unsubscribe(SubscriptionHandle handle) {
    if (!handle || !eventLoop_) return;

    auto* sub = static_cast<Subscription*>(handle);

    esp_event_handler_instance_unregister_with(
        eventLoop_, CROSSPAD_EVENTS, static_cast<int32_t>(sub->type),
        sub->espInstance);

    delete sub;
}

// ── Post helpers ────────────────────────────────────────────────────────

bool EspEventBus::postEvent(EventType type, const void* data, size_t dataSize) {
    if (!eventLoop_) return false;

    esp_err_t err = esp_event_post_to(
        eventLoop_, CROSSPAD_EVENTS, static_cast<int32_t>(type),
        data, dataSize, portMAX_DELAY);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to post %s: %s", getEventName(type), esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool EspEventBus::sendEvent(EventType type, const void* data, size_t dataSize) {
    if (!eventLoop_) return false;

    esp_err_t err = esp_event_isr_post_to(
        eventLoop_, CROSSPAD_EVENTS, static_cast<int32_t>(type),
        data, dataSize, nullptr);

    return err == ESP_OK;
}

// ── Async post methods ──────────────────────────────────────────────────

bool EspEventBus::postNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) {
    NoteOnData data = { .channel = channel, .note = note, .velocity = velocity, .source = source };
    ESP_LOGD(TAG, "Post NoteOn: ch=%d note=%d vel=%d src=%s",
             channel, note, velocity, getSourceName(source));
    return postEvent(EventType::NoteOn, &data, sizeof(data));
}

bool EspEventBus::postNoteOff(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) {
    NoteOffData data = { .channel = channel, .note = note, .velocity = velocity, .source = source };
    ESP_LOGD(TAG, "Post NoteOff: ch=%d note=%d vel=%d src=%s",
             channel, note, velocity, getSourceName(source));
    return postEvent(EventType::NoteOff, &data, sizeof(data));
}

bool EspEventBus::postPadPressed(uint8_t padIdx, bool isOn, uint8_t velocity, EventSource source) {
    PadPressedData data = { .padIdx = padIdx, .isOn = isOn, .velocity = velocity, .source = source };
    ESP_LOGD(TAG, "Post PadPressed: pad=%d on=%d vel=%d src=%s",
             padIdx, isOn, velocity, getSourceName(source));
    return postEvent(EventType::PadPressed, &data, sizeof(data));
}

bool EspEventBus::postPadReleased(uint8_t padIdx, EventSource source) {
    PadReleasedData data = { .padIdx = padIdx, .source = source };
    ESP_LOGD(TAG, "Post PadReleased: pad=%d src=%s", padIdx, getSourceName(source));
    return postEvent(EventType::PadReleased, &data, sizeof(data));
}

bool EspEventBus::postAppChanged(const char* appName, EventSource source) {
    AppChangedData data;
    data.setName(appName);
    data.source = source;
    ESP_LOGD(TAG, "Post AppChanged: app=%s src=%s", appName, getSourceName(source));
    return postEvent(EventType::AppChanged, &data, sizeof(data));
}

bool EspEventBus::postTempoChanged(float bpm, EventSource source) {
    TempoChangedData data = { .bpm = bpm, .source = source };
    ESP_LOGD(TAG, "Post TempoChanged: bpm=%.1f src=%s", bpm, getSourceName(source));
    return postEvent(EventType::TempoChanged, &data, sizeof(data));
}

bool EspEventBus::postLiveModeChanged(bool enabled, EventSource source) {
    LiveModeChangedData data = { .enabled = enabled, .source = source };
    ESP_LOGD(TAG, "Post LiveModeChanged: enabled=%d src=%s", enabled, getSourceName(source));
    return postEvent(EventType::LiveModeChanged, &data, sizeof(data));
}

bool EspEventBus::postGuiUpdateRequired(EventSource source) {
    ESP_LOGD(TAG, "Post GuiUpdateRequired: src=%s", getSourceName(source));
    return postEvent(EventType::GuiUpdateRequired, &source, sizeof(source));
}

bool EspEventBus::postSamplePlayback(uint8_t note, uint8_t wavIdx, bool isOn, EventSource source) {
    SamplePlaybackData data = { .note = note, .wavIdx = wavIdx, .isOn = isOn, .source = source };
    ESP_LOGD(TAG, "Post SamplePlayback: note=%d wav=%d on=%s src=%s",
             note, wavIdx, isOn ? "true" : "false", getSourceName(source));
    return postEvent(EventType::SamplePlayback, &data, sizeof(data));
}

// ── Sync send methods ───────────────────────────────────────────────────

bool EspEventBus::sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) {
    NoteOnData data = { .channel = channel, .note = note, .velocity = velocity, .source = source };
    ESP_LOGD(TAG, "Send NoteOn (sync): ch=%d note=%d vel=%d src=%s",
             channel, note, velocity, getSourceName(source));
    return sendEvent(EventType::NoteOn, &data, sizeof(data));
}

bool EspEventBus::sendNoteOff(uint8_t channel, uint8_t note, EventSource source) {
    NoteOffData data = { .channel = channel, .note = note, .velocity = 0, .source = source };
    ESP_LOGD(TAG, "Send NoteOff (sync): ch=%d note=%d src=%s",
             channel, note, getSourceName(source));
    return sendEvent(EventType::NoteOff, &data, sizeof(data));
}

} // namespace crosspad

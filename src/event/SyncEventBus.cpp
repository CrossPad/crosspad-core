/**
 * @file SyncEventBus.cpp
 * @brief Synchronous event bus — direct dispatch, no queue.
 */

#include "crosspad/event/SyncEventBus.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace crosspad {

bool SyncEventBus::init() {
    if (initialized_) return true;
    initialized_ = true;
    printf("[EventBus] SyncEventBus initialized (direct dispatch)\n");
    return true;
}

SubscriptionHandle SyncEventBus::subscribe(EventType type, EventHandler handler, void* user_ctx) {
    if (!initialized_) {
        if (!init()) return nullptr;
    }

    auto* sub = new Subscription{type, handler, user_ctx};
    std::lock_guard<std::mutex> lock(mutex_);
    subs_.push_back(sub);
    return static_cast<SubscriptionHandle>(sub);
}

void SyncEventBus::unsubscribe(SubscriptionHandle handle) {
    if (!handle) return;

    auto* sub = static_cast<Subscription*>(handle);
    std::lock_guard<std::mutex> lock(mutex_);
    subs_.erase(std::remove(subs_.begin(), subs_.end(), sub), subs_.end());
    delete sub;
}

void SyncEventBus::dispatch(EventType type, const void* data, size_t dataSize) {
    (void)dataSize;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto* sub : subs_) {
        if (sub->type == type) {
            sub->handler(data, type, sub->userCtx);
        }
    }
}

// ── Async post methods ──────────────────────────────────────────────────────

bool SyncEventBus::postNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) {
    NoteOnData data{channel, note, velocity, source};
    dispatch(EventType::NoteOn, &data, sizeof(data));
    return true;
}

bool SyncEventBus::postNoteOff(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) {
    NoteOffData data{channel, note, velocity, source};
    dispatch(EventType::NoteOff, &data, sizeof(data));
    return true;
}

bool SyncEventBus::postPadPressed(uint8_t padIdx, bool isOn, uint8_t velocity, EventSource source) {
    PadPressedData data{padIdx, isOn, velocity, source};
    dispatch(EventType::PadPressed, &data, sizeof(data));
    return true;
}

bool SyncEventBus::postPadReleased(uint8_t padIdx, EventSource source) {
    PadReleasedData data{padIdx, source};
    dispatch(EventType::PadReleased, &data, sizeof(data));
    return true;
}

bool SyncEventBus::postAppChanged(const char* appName, EventSource source) {
    AppChangedData data;
    data.setName(appName);
    data.source = source;
    dispatch(EventType::AppChanged, &data, sizeof(data));
    return true;
}

bool SyncEventBus::postTempoChanged(float bpm, EventSource source) {
    TempoChangedData data{bpm, source};
    dispatch(EventType::TempoChanged, &data, sizeof(data));
    return true;
}

bool SyncEventBus::postLiveModeChanged(bool enabled, EventSource source) {
    LiveModeChangedData data{enabled, source};
    dispatch(EventType::LiveModeChanged, &data, sizeof(data));
    return true;
}

bool SyncEventBus::postGuiUpdateRequired(EventSource source) {
    dispatch(EventType::GuiUpdateRequired, &source, sizeof(source));
    return true;
}

bool SyncEventBus::postSamplePlayback(uint8_t note, uint8_t wavIdx, bool isOn, EventSource source, uint8_t velocity) {
    SamplePlaybackData data{note, wavIdx, isOn, velocity, source};
    dispatch(EventType::SamplePlayback, &data, sizeof(data));
    return true;
}

// ── Sync send methods ───────────────────────────────────────────────────────

bool SyncEventBus::sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) {
    NoteOnData data{channel, note, velocity, source};
    dispatch(EventType::NoteOn, &data, sizeof(data));
    return true;
}

bool SyncEventBus::sendNoteOff(uint8_t channel, uint8_t note, EventSource source) {
    NoteOffData data{channel, note, 0, source};
    dispatch(EventType::NoteOff, &data, sizeof(data));
    return true;
}

} // namespace crosspad

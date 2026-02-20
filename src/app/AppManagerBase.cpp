#include "crosspad/app/AppManagerBase.hpp"

namespace crosspad {

void AppManagerBase::initEventForwarding(IEventBus& eventBus) {
    noteOnSub_ = eventBus.subscribe(EventType::NoteOn,
        [](const void* event_data, EventType, void* ctx) {
            auto* mgr = static_cast<AppManagerBase*>(ctx);
            auto* data = static_cast<const NoteOnData*>(event_data);
            if (mgr->currentApp_) {
                mgr->currentApp_->onNoteOn(data->channel, data->note, data->velocity);
            }
        }, this);

    noteOffSub_ = eventBus.subscribe(EventType::NoteOff,
        [](const void* event_data, EventType, void* ctx) {
            auto* mgr = static_cast<AppManagerBase*>(ctx);
            auto* data = static_cast<const NoteOffData*>(event_data);
            if (mgr->currentApp_) {
                mgr->currentApp_->onNoteOff(data->channel, data->note);
            }
        }, this);

    padPressedSub_ = eventBus.subscribe(EventType::PadPressed,
        [](const void* event_data, EventType, void* ctx) {
            auto* mgr = static_cast<AppManagerBase*>(ctx);
            auto* data = static_cast<const PadPressedData*>(event_data);
            if (mgr->currentApp_) {
                mgr->currentApp_->onPadPressed(data->padIdx, data->isOn, data->velocity);
            }
        }, this);

    padReleasedSub_ = eventBus.subscribe(EventType::PadReleased,
        [](const void* event_data, EventType, void* ctx) {
            auto* mgr = static_cast<AppManagerBase*>(ctx);
            auto* data = static_cast<const PadReleasedData*>(event_data);
            if (mgr->currentApp_) {
                mgr->currentApp_->onPadReleased(data->padIdx);
            }
        }, this);
}

void AppManagerBase::deinitEventForwarding(IEventBus& eventBus) {
    if (noteOnSub_)      { eventBus.unsubscribe(noteOnSub_);      noteOnSub_ = nullptr; }
    if (noteOffSub_)     { eventBus.unsubscribe(noteOffSub_);     noteOffSub_ = nullptr; }
    if (padPressedSub_)  { eventBus.unsubscribe(padPressedSub_);  padPressedSub_ = nullptr; }
    if (padReleasedSub_) { eventBus.unsubscribe(padReleasedSub_); padReleasedSub_ = nullptr; }
}

void AppManagerBase::update() {
    if (currentApp_) {
        currentApp_->onAppUpdate();
    }
}

const char* AppManagerBase::getCurrentAppName() const {
    return currentApp_ ? currentApp_->getName() : "None";
}

bool AppManagerBase::switchToNextApp() {
    size_t count = getAppCount();
    if (count <= 1) return false;

    int next = (getCurrentAppIndex() + 1) % static_cast<int>(count);
    return switchToApp(next);
}

bool AppManagerBase::switchToPrevApp() {
    size_t count = getAppCount();
    if (count <= 1) return false;

    int prev = (getCurrentAppIndex() - 1 + static_cast<int>(count)) % static_cast<int>(count);
    return switchToApp(prev);
}

} // namespace crosspad

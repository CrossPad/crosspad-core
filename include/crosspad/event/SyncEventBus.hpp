#pragma once

/**
 * @file SyncEventBus.hpp
 * @brief Synchronous (direct-dispatch) implementation of IEventBus.
 *
 * All post/send methods call registered handlers synchronously on the
 * calling thread. No queue, no dedicated dispatch task.
 *
 * Suitable for single-threaded or non-RTOS platforms (PC, Linux, tests).
 * Uses only std::mutex + std::vector — no platform dependencies.
 */

#include "crosspad/event/IEventBus.hpp"
#include <vector>
#include <mutex>

namespace crosspad {

class SyncEventBus : public IEventBus {
public:
    SyncEventBus() = default;
    ~SyncEventBus() override = default;

    bool init() override;

    SubscriptionHandle subscribe(EventType type, EventHandler handler, void* user_ctx = nullptr) override;
    void unsubscribe(SubscriptionHandle handle) override;

    bool postNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) override;
    bool postNoteOff(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) override;
    bool postPadPressed(uint8_t padIdx, bool isOn, uint8_t velocity, EventSource source = EventSource::PadManager) override;
    bool postPadReleased(uint8_t padIdx, EventSource source = EventSource::PadManager) override;
    bool postAppChanged(const char* appName, EventSource source = EventSource::System) override;
    bool postTempoChanged(float bpm, EventSource source = EventSource::SequencerCallback) override;
    bool postLiveModeChanged(bool enabled, EventSource source = EventSource::System) override;
    bool postGuiUpdateRequired(EventSource source = EventSource::Gui) override;
    bool postSamplePlayback(uint8_t note, uint8_t wavIdx, bool isOn, EventSource source = EventSource::SamplePlayback, uint8_t velocity = 127,
                            int8_t transpose = 0, uint8_t key = kSamplePlaybackOwnKey) override;
    bool postMotionGesture(MotionGesture gesture, uint8_t orientation = 0, EventSource source = EventSource::Imu) override;

    bool sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source = EventSource::System) override;
    bool sendNoteOff(uint8_t channel, uint8_t note, EventSource source = EventSource::System) override;

private:
    struct Subscription {
        EventType type;
        EventHandler handler;
        void* userCtx;
    };

    void dispatch(EventType type, const void* data, size_t dataSize);

    std::vector<Subscription*> subs_;
    std::mutex mutex_;
    bool initialized_ = false;
};

} // namespace crosspad

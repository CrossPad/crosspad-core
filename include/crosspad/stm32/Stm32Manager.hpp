#pragma once

#include <cstdint>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "crosspad/protocol/Stm32CommandSender.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/status/CrosspadStatus.hpp"
#include "crosspad/stm32/Stm32ConfigSync.hpp"
#include "crosspad/audio/SpeakerAmpGate.hpp"

namespace crosspad {

/**
 * @brief Portable STM32 co-processor manager
 *
 * Runs a FreeRTOS maintenance task that periodically sends keep-alive
 * and routing matrix messages to the STM32. Routing is read from
 * CrosspadSettings (not hardcoded).
 *
 * Platform-specific actions (GPIO reset, button check) are injected
 * via callbacks.
 */
class Stm32Manager {
public:
    using ResetCallback = std::function<void()>;
    using DfuButtonCheckCallback = std::function<bool()>;

    Stm32Manager() = default;

    /**
     * @brief Store dependencies (does not start the task)
     */
    void init(Stm32CommandSender& cmdSender,
              CrosspadSettings* settings,
              CrosspadStatus* status);

    /**
     * @brief Send initial routing + keepalive and create maintenance task
     */
    void start();

    /**
     * @brief Stop maintenance task
     */
    void stop();

    bool isRunning() const { return running_; }

    /**
     * @brief Send brightness command (only on change)
     */
    void setBrightness(uint8_t brightness);

    /**
     * @brief Request a keep-alive to be sent on next loop iteration
     */
    void requestKeepAlive() { keepAliveRequested_ = true; }

    /**
     * @brief Report ESP-side user input (touch, GUI navigation) to the STM.
     *
     * Touch never reaches the STM pad scan, so without this the idle timer would
     * only see pads/encoder/buttons. Flags only — the maintenance task does the
     * I2C write, so this is safe from LVGL/input callbacks.
     */
    void notifyUserActivity() { userActivityPending_ = true; }

    /**
     * @brief Blocking DFU poll loop — checks status and waits for button
     */
    void checkDfuMode();

    void setResetCallback(ResetCallback cb) { resetCb_ = std::move(cb); }
    void setDfuButtonCheckCallback(DfuButtonCheckCallback cb) { dfuButtonCb_ = std::move(cb); }
    void setConfigSync(Stm32ConfigSync* sync) { configSync_ = sync; }
    Stm32ConfigSync* configSync() const { return configSync_; }

    /**
     * @brief Level-driven speaker amp policy.
     *
     * The audio thread feeds it peaks via submitLevel(); the maintenance loop
     * polls it and writes REG_CTL_AUDIO_EN only when the decision changes, so
     * the amp draws no current while nothing is playing.
     */
    SpeakerAmpGate& ampGate() { return ampGate_; }

    /**
     * @brief Forget what we believe the STM's amp register holds.
     *
     * The STM gates the amp off by itself when it dims into LIGHT sleep, without
     * touching REG_CTL_AUDIO_EN. Our cached value therefore still reads "on" and
     * the change-detecting write never fires — the speakers stay dead after wake
     * while headphones keep playing. Call this on every wake notify so the next
     * poll re-asserts the gate unconditionally.
     */
    void invalidateAmpState() { ampResyncPending_ = true; }

private:
    static void taskEntry(void* param);
    void maintenanceLoop();

    Stm32CommandSender* cmdSender_ = nullptr;
    CrosspadSettings* settings_ = nullptr;
    CrosspadStatus* status_ = nullptr;

    TaskHandle_t taskHandle_ = nullptr;
    bool running_ = false;
    volatile bool keepAliveRequested_ = false;
    volatile bool userActivityPending_ = false;
    volatile bool ampResyncPending_ = false;
    uint8_t lastBrightness_ = 0xFF; // impossible initial value → first call always sends

    ResetCallback resetCb_;
    DfuButtonCheckCallback dfuButtonCb_;

    Stm32ConfigSync* configSync_ = nullptr;
    int lastBootCount_ = -1;   // <0 = never synced

    SpeakerAmpGate ampGate_;
    int ampApplied_ = -1;      // <0 = unknown, forces the next decision to be written
};

} // namespace crosspad

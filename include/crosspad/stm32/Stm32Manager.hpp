#pragma once

#include <cstdint>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "crosspad/protocol/Stm32CommandSender.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/status/CrosspadStatus.hpp"
#include "crosspad/stm32/Stm32ConfigSync.hpp"

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
     * @brief Blocking DFU poll loop — checks status and waits for button
     */
    void checkDfuMode();

    void setResetCallback(ResetCallback cb) { resetCb_ = std::move(cb); }
    void setDfuButtonCheckCallback(DfuButtonCheckCallback cb) { dfuButtonCb_ = std::move(cb); }
    void setConfigSync(Stm32ConfigSync* sync) { configSync_ = sync; }
    Stm32ConfigSync* configSync() const { return configSync_; }

private:
    static void taskEntry(void* param);
    void maintenanceLoop();

    Stm32CommandSender* cmdSender_ = nullptr;
    CrosspadSettings* settings_ = nullptr;
    CrosspadStatus* status_ = nullptr;

    TaskHandle_t taskHandle_ = nullptr;
    bool running_ = false;
    volatile bool keepAliveRequested_ = false;
    uint8_t lastBrightness_ = 0xFF; // impossible initial value → first call always sends

    ResetCallback resetCb_;
    DfuButtonCheckCallback dfuButtonCb_;

    Stm32ConfigSync* configSync_ = nullptr;
    int lastBootCount_ = -1;   // <0 = never synced
};

} // namespace crosspad

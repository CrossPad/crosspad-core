#include "crosspad/stm32/Stm32Manager.hpp"
#include <cstdio>

namespace crosspad {

void Stm32Manager::init(Stm32CommandSender& cmdSender,
                         CrosspadSettings* settings,
                         CrosspadStatus* status) {
    cmdSender_ = &cmdSender;
    settings_ = settings;
    status_ = status;
}

void Stm32Manager::start() {
    if (running_) return;

    // Send initial routing from settings and keepalive
    if (cmdSender_ && settings_) {
        cmdSender_->sendRoutingMatrix(settings_->stmRouting);
        cmdSender_->sendKeepAlive();
    }

    BaseType_t ret = xTaskCreate(
        taskEntry,
        "stm32_maint",
        3072,
        this,
        3,
        &taskHandle_
    );

    if (ret == pdPASS) {
        running_ = true;
    }
}

void Stm32Manager::stop() {
    if (!running_) return;

    if (taskHandle_) {
        vTaskDelete(taskHandle_);
        taskHandle_ = nullptr;
    }
    running_ = false;
}

void Stm32Manager::setBrightness(uint8_t brightness) {
    if (brightness == lastBrightness_) return;
    if (cmdSender_) {
        cmdSender_->sendBrightness(brightness);
        lastBrightness_ = brightness;
    }
}

void Stm32Manager::checkDfuMode() {
    if (!status_) return;
    if (!status_->stm32.isDfuMode) return;

    printf("\nSTM is in DFU mode, waiting for it to finish");
    while (status_->stm32.isDfuMode) {
        printf("\n\nSTM32 is in DFU mode, press encoder to exit. Use USB DFU tool to flash firmware\n\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (dfuButtonCb_ && dfuButtonCb_()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (resetCb_) resetCb_();
            while (true) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
}

void Stm32Manager::taskEntry(void* param) {
    auto* self = static_cast<Stm32Manager*>(param);
    self->maintenanceLoop();
    vTaskDelete(nullptr);
}

void Stm32Manager::maintenanceLoop() {
    TickType_t lastKeepAlive = 0;
    TickType_t lastRouting = 0;

    const TickType_t keepAliveInterval = pdMS_TO_TICKS(5000);
    const TickType_t routingInterval = pdMS_TO_TICKS(3000);

    while (true) {
        TickType_t now = xTaskGetTickCount();

        // Keep-alive: every 5s or on request
        if (keepAliveRequested_ || (now - lastKeepAlive >= keepAliveInterval)) {
            if (cmdSender_) cmdSender_->sendKeepAlive();
            lastKeepAlive = now;
            keepAliveRequested_ = false;
        }

        // Routing matrix from settings: every 3s
        if (now - lastRouting >= routingInterval) {
            if (cmdSender_ && settings_) {
                cmdSender_->sendRoutingMatrix(settings_->stmRouting);
            }
            lastRouting = now;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

} // namespace crosspad

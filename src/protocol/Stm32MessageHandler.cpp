#include "crosspad/protocol/Stm32MessageHandler.hpp"
#include "crosspad/status/CrosspadStatus.hpp"
#include "crosspad/pad/PadManager.hpp"

namespace crosspad {

void Stm32MessageHandler::init(PadManager& padManager, CrosspadStatus& status) {
    padManager_ = &padManager;
    status_ = &status;
}

void Stm32MessageHandler::handleMessage(const uint8_t* data, unsigned size) {
    if (size < 3) return;
    if (!status_ || !padManager_) return;

    uint8_t cmd = data[2];
    switch (cmd) {

    case CROSSPAD_CMD_BRIGHTNESS:
        break;

    case CROSSPAD_CMD_ENCODER_COUNTER:
        if (size < 5) break;
        // STM r20 packs raw LPTIM1 CNT mod 64 directly (bit6 = button,
        // bits 5..0 = counter). LPTIM samples 4 transitions per detent so
        // CNT steps ~4 per click — keep the byte as-is so LVGL receives a
        // diff per detent rather than per transition.
        input.encoderTop = (data[3] & 0x3F);
        input.encoderPressed = (data[3] >> 6) & 0x01;
        break;

    case CROSSPAD_CMD_BUTTONS_DIGITAL:
        if (size < 7) break;
        parseDigitalButtons(data, size);
        break;

    case CROSSPAD_CMD_BUTTONS_ANALOG:
        // Analog pressure values — currently just decoded, not dispatched
        break;

    case CROSSPAD_CMD_STATUS_REGISTER:
        if (size < 6) break;
        status_->stm32.SetStm32Status(data[3], data[4]);
        break;

    case CROSSPAD_CMD_ROUTING_MATRIX:
        break;

    case CROSSPAD_CMD_VOLTAGES_MV:
        if (size < 10) break;
        status_->stm32.unpackVoltages(&data[3]);
        status_->batteryPct = Stm32Status::calculateBatteryPct(status_->stm32.VBAT);
        break;

    case CROSSPAD_CMD_KEEP_ALIVE:
        break;

    case CROSSPAD_CMD_USB_MODE:
        if (usbModeCb_ && size >= 5) {
            usbModeCb_(data[3]);
        }
        break;

    case CROSSPAD_CMD_VERSION_REPLY:
        // [pcb_rev, api_ver] — currently informational, no consumer
        if (size < 6) break;
        break;

    case CROSSPAD_CMD_DFU_REQUEST:
        if (size < 5) break;
        // Unified semantics in both directions:
        //   data[3] == 1 -> STM32 is going to / requested to enter DFU
        //   data[3] == 0 -> ESP is going to / requested to enter DFU
        status_->stm32.isDfuMode = (data[3] == 1);
        if (dfuCb_) {
            dfuCb_(data[3] == 1);
        }
        break;

    case CROSSPAD_CMD_POWEROFF:
        break;

    default:
        break;
    }
}

void Stm32MessageHandler::parseDigitalButtons(const uint8_t* data, unsigned size) {
    if (!padManager_) return;

    // Button index decode table: 3 bytes × 7 bits each → up to 21 button indices
    static const int8_t buttonsidx[4][8] = {
        {-1, 1, 2, 3, 4, 5, 6, 7},
        {-1, 8, 9, 10, 11, 12, 13, 14},
        {-1, 15, 16, 17, 18, 19, 20, 21},
        {-1, 22, 23, 24, 25, 26, 27, 28}
    };

    bool oldPresses[16];
    memcpy(oldPresses, keyPresses_, sizeof(oldPresses));

    for (int i = 0; i < 3; i++) {
        uint8_t x = data[i + 3];
        for (int j = 1; j < 8; j++) {
            int num = buttonsidx[i][j];
            if (num <= 16) {
                keyPresses_[num - 1] = !(x >> (7 - j) & 1);
            }
        }
        if (i == 2) {
            input.powerBtnPressed = x & 1;
        }
    }

    // Detect state changes and forward to PadManager
    for (int i = 0; i < 16; i++) {
        if (keyPresses_[i] != oldPresses[i]) {
            if (keyPresses_[i]) {
                padManager_->handlePadPress(i, 127);
            } else {
                padManager_->handlePadRelease(i);
            }
        }
    }
}

} // namespace crosspad

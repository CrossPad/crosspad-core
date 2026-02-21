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
        input.encoderTop = (data[3] & 0x3F) / 2; // max 60 on lptimer
        input.encoderPressed = (data[3] >> 6);    // press state at 2nd MSB
        break;

    case CROSSPAD_CMD_BUTTONS_DIGITAL:
        parseDigitalButtons(data, size);
        break;

    case CROSSPAD_CMD_BUTTONS_ANALOG:
        // Analog pressure values — currently just decoded, not dispatched
        break;

    case CROSSPAD_CMD_STATUS_REGISTER:
        status_->stm32.SetStm32Status(data[3], data[4]);
        break;

    case CROSSPAD_CMD_ROUTING_MATRIX:
        break;

    case CROSSPAD_CMD_VOLTAGES_MV:
        status_->stm32.unpackVoltages(&data[3]);
        status_->batteryPct = Stm32Status::calculateBatteryPct(status_->stm32.VBAT);
        break;

    case CROSSPAD_CMD_KEEP_ALIVE:
        break;

    case CROSSPAD_CMD_USB_MODE:
        if (usbModeCb_ && size >= 4) {
            usbModeCb_(data[3]);
        }
        break;

    case CROSSPAD_CMD_DFU_REQUEST:
        status_->stm32.isDfuMode = (data[3] == 1);
        if (dfuCb_) {
            dfuCb_(data[3] == 1); // true = STM32 DFU, false = ESP DFU
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

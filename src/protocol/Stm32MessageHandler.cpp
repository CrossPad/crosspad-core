#include "crosspad/protocol/Stm32MessageHandler.hpp"
#include "crosspad/status/CrosspadStatus.hpp"
#include "crosspad/pad/PadManager.hpp"

namespace crosspad {

void Stm32MessageHandler::init(PadManager& padManager, CrosspadStatus& status) {
    padManager_ = &padManager;
    status_ = &status;
}

void Stm32MessageHandler::handleMessage(const uint8_t* data, unsigned size) {
    ++input.stmFrames;
    if (size < 3) return;
    if (!status_ || !padManager_) return;
    // A DAW host shares the USB MIDI port with tools that speak other SysEx
    // dialects; a foreign manufacturer byte must never reach the command
    // switch below (0x15 is POWEROFF, 0x19 a DFU request).
    if (data[0] != CROSSPAD_SYSEX_START || data[1] != CROSSPAD_SYSEX_MANUFACTURER) return;

    uint8_t cmd = data[2];
    switch (cmd) {

    case CROSSPAD_CMD_BRIGHTNESS:
        break;

    case CROSSPAD_CMD_ENCODER_COUNTER:
        if (size < 5) break;
        // Frame length says which STM generation sent this:
        //   6 bytes → detent index 0..15 plus a sub-detent phase byte.
        //   5 bytes → legacy raw LPTIM CNT 0..63; the detent index and phase
        //     are derived here so downstream code stays firmware-agnostic.
        //     Assumes the legacy encoder also rests at CNT ≡ 1 (mod 4).
        if (size >= 6) {
            input.encoderTop   = (data[3] & 0x0F);
            input.encoderPhase = data[4];
        } else {
            uint8_t cnt  = data[3] & 0x3F;
            uint8_t mod4 = cnt & 0x03;
            uint8_t a, b;
            switch (mod4) {                 // Gray map, rest (mod4==1) = (A,B)=(1,1)
                case 1:  a = 1; b = 1; break;
                case 2:  a = 0; b = 1; break;
                case 3:  a = 0; b = 0; break;
                default: a = 1; b = 0; break;
            }
            input.encoderTop   = ((cnt + 1) >> 2) & 0x0F;
            input.encoderPhase = mod4 | (a << 2) | (b << 3) | (mod4 == 1 ? 0x10 : 0);
        }
        input.encoderPressed = (data[3] >> 6) & 0x01;
        break;

    case CROSSPAD_CMD_BUTTONS_DIGITAL:
        if (size < 7) break;
        parseDigitalButtons(data, size);
        break;

    case CROSSPAD_CMD_BUTTONS_ANALOG:
        // Decoded, not yet dispatched.
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

    case CROSSPAD_CMD_AUDIO_ROUTE:
        /* [F0 7D 1D <sub> <args...> F7] — pass sub+args, strip framing */
        if (audioRouteCb_ && size >= 5) {
            audioRouteCb_(data + 3, size - 4);
        }
        break;

    case CROSSPAD_CMD_DAW:
        /* [F0 7D 1E <sub> <args...> F7] — same framing as AUDIO_ROUTE */
        if (dawCb_ && size >= 5) {
            dawCb_(data + 3, size - 4);
        }
        break;

    case CROSSPAD_CMD_COMPANION:
        /* [F0 7D 1F <sub> <ascii...> F7] — same framing as AUDIO_ROUTE */
        if (companionCb_ && size >= 5) {
            companionCb_(data + 3, size - 4);
        }
        break;

    case CROSSPAD_CMD_VERSION_REPLY:
        // [pcb_rev, api_ver] — informational, no consumer yet.
        if (size < 6) break;
        break;

    case CROSSPAD_CMD_DFU_REQUEST:
        if (size < 5) break;
        // Same in both directions: data[3] == 1 means the STM32 enters DFU,
        // 0 means the ESP does.
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
    ++input.powerBtnFrames;
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
            input.powerBtnRawByte = x;
        }
    }

    // Presses do NOT call handlePadPress(): this path carries no velocity
    // byte, and a fixed 127 would clobber what the MIDI note-on pipe already
    // wrote. Releases do go through handlePadRelease(), which cleans up the
    // LED and EventBus state and sends the NoteOff.
    for (int i = 0; i < 16; i++) {
        if (keyPresses_[i] != oldPresses[i]) {
            if (!keyPresses_[i]) {
                padManager_->handlePadRelease(i);
            }
        }
    }
}

} // namespace crosspad

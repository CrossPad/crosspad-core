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

    case CROSSPAD_CMD_ENCODER_COUNTER: {
        if (size < 5) break;
        // Frame length says which STM generation sent this:
        //   6 bytes → detent index 0..15 plus a sub-detent phase byte with
        //     the live A/B lines, which EncoderDetents decodes.
        //   5 bytes → a rev 1.x STM's raw LPTIM count 0..60 and nothing else:
        //     no A/B lines, so the steps come from the count. It moves
        //     kLegacyCountsPerDetent per click (PCB 1.9: every click 2).
        int steps;
        if (size >= 6) {
            input.encoderTop   = (data[3] & 0x0F);
            input.encoderPhase = data[4];
            steps = encoderDetents_.update(input.encoderPhase);
        } else {
            const uint8_t cnt  = data[3] & 0x3F;
            steps = legacyEncoderSteps(cnt);
            input.encoderTop   = (legacyDetent_ / kLegacyCountsPerDetent) & 0x0F;
            input.encoderPhase = cnt & 0x03;
        }
        input.encoderPressed = (data[3] >> 6) & 0x01;
        if (steps) {
            encoderSteps_.fetch_add(steps);
            encoderDetent_.fetch_add(steps);
        }
        break;
    }

    case CROSSPAD_CMD_BUTTONS_DIGITAL:
        if (size < 7) break;
        parseDigitalButtons(data, size);
        break;

    case CROSSPAD_CMD_BUTTONS_ANALOG:
        // Decoded, not yet dispatched.
        break;

    case CROSSPAD_CMD_STATUS_REGISTER:
        // A rev 1.x STM built before CMD_VERSION_REPLY answers the version
        // request with [F0 7D pcb api F7]: the PCB number where the command
        // byte belongs. PCB 1.9 is 0x13, and a real status frame is 7 bytes.
        if (size == 5) {
            if (versionReplyCb_) versionReplyCb_(data[2], data[3]);
            break;
        }
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
        // [pcb_rev, api_ver]
        if (size < 6) break;
        if (versionReplyCb_) versionReplyCb_(data[3], data[4]);
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

int Stm32MessageHandler::legacyEncoderSteps(uint8_t cnt) {
    // The count runs 0..kLegacyCountMax and wraps, an odd number of states,
    // so it is unwrapped into a position whose detents keep one parity. The
    // first report is the knob's first count out of a detent: detents are
    // the other parity. Steps are taken where the knob lands in one, from
    // the last, so a wobble in a detent -- a count out and back -- is none.
    constexpr int kStates = kLegacyCountMax + 1;
    if (legacyRaw_ < 0) {
        legacyRaw_ = cnt;
        legacyPos_ = cnt;
        legacyDetentParity_ = (cnt + 1) & 1;
        return 0;
    }
    int d = (cnt - legacyRaw_) % kStates;
    if (d < 0) d += kStates;
    if (d > kStates / 2) d -= kStates;
    legacyRaw_ = cnt;
    legacyPos_ += d;
    if ((legacyPos_ & 1) != legacyDetentParity_) return 0;
    if (!legacyHaveDetent_) {
        legacyHaveDetent_ = true;
        legacyDetent_ = legacyPos_;
        return 0;
    }
    const int steps = (legacyPos_ - legacyDetent_) / kLegacyCountsPerDetent;
    legacyDetent_ = legacyPos_;
    return steps;
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

    // Without a platform callback, presses do NOT call handlePadPress(): this
    // path carries no velocity byte. Releases go through handlePadRelease(),
    // which cleans up the LED and EventBus state and sends the NoteOff.
    for (int i = 0; i < 16; i++) {
        if (keyPresses_[i] == oldPresses[i]) continue;
        if (padEdgeCb_) {
            padEdgeCb_(static_cast<uint8_t>(i), keyPresses_[i]);
        } else if (!keyPresses_[i]) {
            padManager_->handlePadRelease(i);
        }
    }
}

} // namespace crosspad

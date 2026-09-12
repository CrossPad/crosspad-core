#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include "crosspad/protocol/SysExProtocol.h"

namespace crosspad {

// Forward declarations
class CrosspadStatus;
class PadManager;

/**
 * @brief Portable STM32 SysEx protocol message handler
 *
 * Decodes messages from the STM32 co-processor and dispatches
 * to PadManager / CrosspadStatus. Platform-specific actions
 * (DFU, ESP restart) are handled via callbacks.
 */
class Stm32MessageHandler {
public:
    /// Callback for platform-specific DFU mode entry
    /// @param stm32Dfu true = STM32 goes to DFU, false = ESP goes to DFU
    using DfuRequestCallback = std::function<void(bool stm32Dfu)>;

    /// Callback for platform-specific USB mode switch
    /// @param mode USB mode byte from STM32 (0x01=DEFAULT CDC+MIDI, 0x02=AUDIO MIDI+UAC2)
    using UsbModeCallback = std::function<void(uint8_t mode)>;

    /// Callback for platform-specific audio routing control (CROSSPAD_CMD_AUDIO_ROUTE)
    /// @param args Subcommand byte followed by its arguments (SysEx payload after the command byte)
    /// @param len Number of args bytes
    using AudioRouteCallback = std::function<void(const uint8_t* args, unsigned len)>;

    /// Callback for the DAW controller protocol (CROSSPAD_CMD_DAW), same payload shape
    /// as AudioRouteCallback: sub byte followed by its arguments. Owned by whichever
    /// app is showing the DAW surface; cleared (empty function) when it closes.
    using DawCallback = std::function<void(const uint8_t* args, unsigned len)>;

    /// Callback for companion control over SysEx (CROSSPAD_CMD_COMPANION): the
    /// payload after the command byte — sub 0x01 followed by an ASCII verb line.
    /// The platform answers on the same MIDI port with 0x02/0x03 reply chunks.
    using CompanionCallback = std::function<void(const uint8_t* args, unsigned len)>;

    Stm32MessageHandler() = default;

    /**
     * @brief Initialize with dependencies
     * @param padManager Reference to PadManager for pad press/release
     * @param status Reference to CrosspadStatus for status/voltage updates
     */
    void init(PadManager& padManager, CrosspadStatus& status);

    /**
     * @brief Set callback for DFU mode requests (platform-specific)
     */
    void setDfuRequestCallback(DfuRequestCallback cb) { dfuCb_ = std::move(cb); }

    /**
     * @brief Set callback for USB mode switch requests (platform-specific)
     */
    void setUsbModeCallback(UsbModeCallback cb) { usbModeCb_ = std::move(cb); }

    /**
     * @brief Set callback for audio routing control (platform-specific)
     */
    void setAudioRouteCallback(AudioRouteCallback cb) { audioRouteCb_ = std::move(cb); }

    /**
     * @brief Set callback for the DAW controller protocol (app-owned)
     */
    void setDawCallback(DawCallback cb) { dawCb_ = std::move(cb); }

    /**
     * @brief Set callback for companion control over SysEx (platform-specific)
     */
    void setCompanionCallback(CompanionCallback cb) { companionCb_ = std::move(cb); }

    /**
     * @brief Handle a raw SysEx message from STM32
     * @param data Pointer to message bytes (including F0 header)
     * @param size Number of bytes
     */
    void handleMessage(const uint8_t* data, unsigned size);

    /**
     * @brief Parse digital button bitfield from STM32
     * @param data Pointer to message bytes
     * @param size Number of bytes
     */
    void parseDigitalButtons(const uint8_t* data, unsigned size);

    // ── Input state (read by LVGL encoder/keypad drivers) ──

    struct Stm32InputState {
        int8_t  encoderTop = 0;     // centered detent index 0..15 (one step per mech detent)
        uint8_t encoderPhase = 0;   // sub-detent: [1:0]=CNT mod4, [2]=A, [3]=B, [4]=settled gate
        int8_t  encoderLeft = 0;
        bool encoderPressed = false;
        bool powerBtnPressed = false;
        /* Raw third digital-button byte, so the power-button bit can be
         * checked against what the STM actually sends rather than against an
         * assumption about its polarity. */
        uint8_t powerBtnRawByte = 0;
        uint32_t powerBtnFrames = 0;   /* CMD_BUTTONS_DIGITAL frames seen */
        uint32_t stmFrames = 0;        /* any STM32 message dispatched */
    };

    Stm32InputState input;

private:
    PadManager* padManager_ = nullptr;
    CrosspadStatus* status_ = nullptr;
    DfuRequestCallback dfuCb_;
    UsbModeCallback usbModeCb_;
    AudioRouteCallback audioRouteCb_;
    DawCallback dawCb_;
    CompanionCallback companionCb_;
    bool keyPresses_[16] = {};
};

} // namespace crosspad

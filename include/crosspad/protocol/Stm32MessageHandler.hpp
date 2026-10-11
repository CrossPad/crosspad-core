#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include "crosspad/protocol/SysExProtocol.h"
#include "crosspad/stm32/EncoderDetents.hpp"

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

    /// Callback for a pad edge in CMD_BUTTONS_DIGITAL: @p pressed true on a
    /// press, false on a release. Only a UART-only STM (rev 1.x) sends these
    /// frames, and they are its only pad events -- no note-on, no velocity --
    /// so the platform plays them the way it plays an STM note-on.
    using PadEdgeCallback = std::function<void(uint8_t pad, bool pressed)>;

    /// Callback for CROSSPAD_CMD_VERSION_REPLY: what a UART-only STM (rev 1.x)
    /// reports where a rev 2 STM has registers -- its PCB and protocol version.
    using VersionReplyCallback = std::function<void(uint8_t pcbRev, uint8_t apiVersion)>;

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
     * @brief Set callback for pad edges in digital button frames (platform-specific).
     * Unset, a release goes to PadManager::handlePadRelease() and a press is dropped.
     */
    void setPadEdgeCallback(PadEdgeCallback cb) { padEdgeCb_ = std::move(cb); }

    /**
     * @brief Set callback for the STM's version reply (platform-specific)
     */
    void setVersionReplyCallback(VersionReplyCallback cb) { versionReplyCb_ = std::move(cb); }

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
        int8_t  encoderTop = 0;     // the STM's detent index 0..15, from its count -- which slips; steps: takeEncoderSteps()
        uint8_t encoderPhase = 0;   // sub-detent: [1:0]=CNT mod4, [2]=A, [3]=B, [4]=settled gate (rev 1.x: [1:0] only)
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

    /// Detents turned since the last call, + where the STM's count rises:
    /// decoded from the A/B lines of every report as it arrives
    /// (EncoderDetents), never from the count.
    int takeEncoderSteps() { return encoderSteps_.exchange(0); }

    /// Detents turned since boot, the same way -- a MIDI CC takes it & 0x0F.
    int encoderDetent() const { return encoderDetent_.load(); }

private:
    /// A rev 1.x STM's LPTIM counts per encoder click.
    static constexpr int kLegacyCountsPerDetent = 2;
    /// Its count's top: the STM starts the LPTIM with TIM_CHANNEL_ALL (0x3C)
    /// where the HAL takes the period, so it runs 0..60 and wraps.
    static constexpr int kLegacyCountMax = 60;

    /// Steps from a rev 1.x STM's raw count, + where it rises.
    int legacyEncoderSteps(uint8_t cnt);

    EncoderDetents encoderDetents_;
    int  legacyRaw_ = -1;             ///< the last raw count, -1 before the first
    int  legacyPos_ = 0;              ///< the count unwrapped
    int  legacyDetentParity_ = 0;     ///< the parity of legacyPos_ in a detent
    bool legacyHaveDetent_ = false;
    int  legacyDetent_ = 0;           ///< legacyPos_ of the detent the knob was last in
    std::atomic<int> encoderSteps_{0};
    std::atomic<int> encoderDetent_{0};
    PadManager* padManager_ = nullptr;
    CrosspadStatus* status_ = nullptr;
    DfuRequestCallback dfuCb_;
    UsbModeCallback usbModeCb_;
    AudioRouteCallback audioRouteCb_;
    DawCallback dawCb_;
    CompanionCallback companionCb_;
    PadEdgeCallback padEdgeCb_;
    VersionReplyCallback versionReplyCb_;
    bool keyPresses_[16] = {};
};

} // namespace crosspad

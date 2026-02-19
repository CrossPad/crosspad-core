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

    int8_t encoderTop = 0;
    int8_t encoderTopPrev = 0;
    int8_t encoderLeft = 0;
    int8_t encoderLeftPrev = 0;
    bool encoderPressed = false;
    bool powerBtnPressed = false;
    bool keyPresses[16] = {};

private:
    PadManager* padManager_ = nullptr;
    CrosspadStatus* status_ = nullptr;
    DfuRequestCallback dfuCb_;
};

} // namespace crosspad

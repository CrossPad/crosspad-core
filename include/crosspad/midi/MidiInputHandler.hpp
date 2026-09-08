#pragma once
#include "crosspad/midi/IMidiOutput.hpp"
#include <cstdint>

namespace crosspad {

class PadManager;
class IEventBus;
class CrosspadSettings;

/**
 * @brief Portable MIDI input handler and output router
 *
 * Central MIDI routing brain used by both ESP-IDF and Arduino platforms.
 * Platform repos wire their transport callbacks to call the handle* methods,
 * and inject platform-specific IMidiOutput targets via set*Output().
 *
 * Implements IMidiOutput so PadManager can use it directly as its MIDI
 * output — pad presses route to the correct transports via settings.
 *
 * Three input categories:
 *  - External (BLE/USB host): handleNoteOn/Off, handleControlChange, handleAftertouch
 *    → posts to EventBus, updates pad LEDs, optionally forwards to STM32 (BLE2Midi)
 *  - Audio-routed (STM32 UART / USB MIDI in): handleSample*
 *    → posts to EventBus, triggers sample playback, optionally forwards to BLE (Midi2BLE)
 *  - Pad output (IMidiOutput): sendNoteOn/Off etc.
 *    → routes to STM32/USB/BLE based on keypad.send2STM/send2USB/send2BLE
 */
class MidiInputHandler : public IMidiOutput {
public:
    MidiInputHandler() = default;
    ~MidiInputHandler() = default;

    /**
     * @brief Initialize with required dependencies.
     *
     * @param padManager    Reference to PadManager singleton
     * @param eventBus      Reference to IEventBus singleton
     * @param settings      Pointer to CrosspadSettings (read routing config). May be nullptr.
     */
    void init(PadManager& padManager, IEventBus& eventBus, CrosspadSettings* settings);

    /// Set the STM32 UART MIDI output target (nullable)
    void setStm32Output(IMidiOutput* output);
    /// Set the USB MIDI output target (nullable)
    void setUsbOutput(IMidiOutput* output);
    /// Set the BLE MIDI output target (nullable)
    void setBleOutput(IMidiOutput* output);

    // ── External MIDI input (BLE / USB host / etc.) ────────────────────
    void handleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    void handleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
    void handleControlChange(uint8_t channel, uint8_t controller, uint8_t value);
    void handleAftertouch(uint8_t channel, uint8_t note, uint8_t pressure);

    // ── Audio-routed MIDI input (STM32 UART / USB MIDI in) ─────────────
    void handleSampleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    void handleSampleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
    void handleSampleControlChange(uint8_t channel, uint8_t controller, uint8_t value);
    void handleSampleAftertouch(uint8_t channel, uint8_t note, uint8_t pressure);
    void handleSamplePitchBend(uint8_t channel, int16_t value);
    void handleSampleProgramChange(uint8_t channel, uint8_t program);

    // ── IMidiOutput (pad output routing via settings) ──────────────────
    void sendNoteOn(uint8_t note, uint8_t channel) override;
    void sendNoteOff(uint8_t note, uint8_t channel) override;
    void sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel) override;
    /// Host-protocol replies (CrossPad SysEx catalog) go to every host-facing
    /// output — USB and BLE — regardless of the keypad send flags, which gate
    /// notes, not protocol traffic.
    void sendSysEx(const uint8_t* frame, size_t len) override;
    void sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel) override;
    void sendControlChange(uint8_t controller, uint8_t value, uint8_t channel) override;
    void sendAftertouch(uint8_t note, uint8_t pressure, uint8_t channel) override;
    void sendProgramChange(uint8_t program, uint8_t channel) override;
    void sendPitchBend(int16_t value, uint8_t channel) override;

private:
    PadManager*       padManager_   = nullptr;
    IEventBus*        eventBus_     = nullptr;
    CrosspadSettings* settings_     = nullptr;
    IMidiOutput*      stm32Output_  = nullptr;
    IMidiOutput*      usbOutput_    = nullptr;
    IMidiOutput*      bleOutput_    = nullptr;
};

/// Platform-injected singleton accessor (defined in CrosspadPlatform.cpp)
MidiInputHandler& getMidiInputHandler();

} // namespace crosspad

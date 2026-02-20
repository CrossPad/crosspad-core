#pragma once
#include <cstdint>

namespace crosspad {

class PadManager;
class IEventBus;
class CrosspadSettings;
class IMidiOutput;

/**
 * @brief Portable MIDI input handler
 *
 * Extracts the shared MIDI callback logic that was previously duplicated in
 * each platform's midiCallbacks.cpp. Platform repos wire their transport
 * callbacks to call these methods.
 *
 * Two types of MIDI input are handled:
 *  - External (BLE/USB host): handleNoteOn/Off, handleControlChange, handleAftertouch
 *    → posts to EventBus, forwards to PadManager, optionally routes to forwardTarget
 *  - Audio-routed (STM32 UART / USB MIDI in): handleSampleNoteOn/Off
 *    → posts to EventBus, triggers sample playback via PadManager
 *
 * The optional forwardTarget is a second IMidiOutput used for MIDI routing
 * between transports (e.g. BLE → STM32 forwarding controlled by esp32Routing.BLE2Midi).
 * Pass nullptr if no forwarding is needed.
 */
class MidiInputHandler {
public:
    MidiInputHandler() = default;
    ~MidiInputHandler() = default;

    /**
     * @brief Initialize with required dependencies.
     *
     * @param padManager    Reference to PadManager singleton
     * @param eventBus      Reference to IEventBus singleton
     * @param settings      Pointer to CrosspadSettings (read routing config). May be nullptr.
     * @param forwardTarget Optional MIDI output for routing-based forwarding. May be nullptr.
     */
    void init(PadManager& padManager, IEventBus& eventBus,
              CrosspadSettings* settings, IMidiOutput* forwardTarget = nullptr);

    /**
     * @brief Change the forwarding MIDI target at runtime.
     *
     * Useful when transport configuration changes (e.g. USB mode switch).
     */
    void setForwardTarget(IMidiOutput* target);

    // ── External MIDI input (BLE / USB host / etc.) ────────────────────
    void handleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    void handleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
    void handleControlChange(uint8_t channel, uint8_t controller, uint8_t value);
    void handleAftertouch(uint8_t channel, uint8_t note, uint8_t pressure);

    // ── Audio-routed MIDI input (STM32 UART / USB MIDI in) ─────────────
    void handleSampleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    void handleSampleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);

private:
    PadManager*      padManager_   = nullptr;
    IEventBus*       eventBus_     = nullptr;
    CrosspadSettings* settings_    = nullptr;
    IMidiOutput*     forwardTarget_ = nullptr;
};

/// Platform-injected singleton accessor (defined in CrosspadPlatform.cpp)
MidiInputHandler& getMidiInputHandler();

} // namespace crosspad

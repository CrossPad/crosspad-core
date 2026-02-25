#pragma once

#include <cstdint>

namespace crosspad {

// Forward declarations
class IEventBus;
class IClock;
class IMidiOutput;
class ILedStrip;
class IKeyValueStore;
class CrosspadSettings;
class CrosspadStatus;
class PadManager;
class PadLedController;
class PadAnimator;

/**
 * @brief Platform-provided configuration for crosspad-core initialization.
 *
 * Each platform creates its own concrete implementations of the interfaces,
 * fills this struct, and calls crosspad_platform_init(). The common init
 * function wires PadAnimator, PadLedController, PadManager, and Settings.
 */
struct CrosspadPlatformConfig {
    // Required — interface implementations
    IEventBus*        eventBus        = nullptr;
    IClock*           clock           = nullptr;
    ILedStrip*        ledStrip        = nullptr;
    IMidiOutput*      midiOutput      = nullptr;

    // Required — platform-owned core objects
    PadManager*       padManager       = nullptr;
    PadLedController* padLedController = nullptr;
    PadAnimator*      padAnimator      = nullptr;

    // Optional
    IKeyValueStore*   kvStore   = nullptr;  ///< nullptr = skip settings persistence
    CrosspadSettings* settings  = nullptr;  ///< nullptr = use getInstance()
    CrosspadStatus*   status    = nullptr;  ///< nullptr = use static default
    uint8_t           padCount  = 16;
};

/**
 * @brief Common platform initialization.
 *
 * Performs the shared init sequence that every platform needs:
 * 1. eventBus->init()
 * 2. kvStore->init() + settings->loadFrom() + settings->saveTo() (if kvStore provided)
 * 3. padAnimator->init(ledStrip, clock)
 * 4. padLedController->init(ledStrip, eventBus, settings, padCount)
 * 5. padManager->init(padLedController, padAnimator, midiOutput, eventBus, settings, status)
 * 6. padManager->begin() + padLedController->begin()
 *
 * @return true on success, false if required fields are missing
 */
bool crosspad_platform_init(const CrosspadPlatformConfig& config);

/**
 * @brief Re-wire MIDI output at runtime (e.g., when swapping null stub for real MIDI).
 *
 * Re-inits PadManager with the new MIDI output and calls begin().
 */
void crosspad_platform_set_midi(IMidiOutput& midi);

} // namespace crosspad

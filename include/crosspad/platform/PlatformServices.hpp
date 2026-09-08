// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file PlatformServices.hpp
 * @brief Centralized service registry for all CrossPad platform services.
 *
 * Replaces per-platform singleton getter boilerplate. Platforms create their
 * concrete implementations and pass them via CrosspadPlatformConfig — the
 * common init function populates this registry. Runtime service swaps (e.g.,
 * connecting MIDI after init) use the typed setters, which also auto-update
 * PlatformCapabilities flags.
 *
 * Getter functions (getClock(), getMidiOutput(), etc.) are defined once in
 * PlatformServices.cpp — platforms no longer need to implement them.
 */

#include <cstdint>

namespace crosspad {

// Forward declarations — no heavy includes in this header
class IEventBus;
class IClock;
class ILedStrip;
class IMidiOutput;
class IAudioOutput;
class IAudioInput;
class IAudioModule;
class ISynthEngine;
class IKeyValueStore;
class IVibration;
class IImu;
class IHttpClient;
class ISettingsUI;
class IKitManager;
class IBleMidi;
class PadManager;
class PadLedController;
class PadAnimator;
class CrosspadSettings;
class CrosspadStatus;
class SequencerController;
class ISequencerEngine;
class PitchedInstrument;
class Stm32MessageHandler;

/**
 * @brief Central registry of all platform service pointers.
 *
 * Core services are populated by crosspad_platform_init(). Optional services
 * default to null-object stubs (NullVibration, NullImu, NullMidiOutput,
 * NullAudioOutput) so getters never return null.
 *
 * Use typed setters for runtime swaps — they auto-update capability flags.
 */
struct PlatformServices {
    // ── Core services (populated by crosspad_platform_init) ─────────────
    IEventBus*        eventBus        = nullptr;
    IClock*           clock           = nullptr;
    ILedStrip*        ledStrip        = nullptr;
    IMidiOutput*      midiOutput      = nullptr;  // defaults to NullMidiOutput
    PadManager*       padManager      = nullptr;
    PadLedController* padLedController = nullptr;
    PadAnimator*      padAnimator     = nullptr;
    IKeyValueStore*   kvStore         = nullptr;
    CrosspadSettings* settings        = nullptr;
    CrosspadStatus*   status          = nullptr;

    // ── Optional services (default to null stubs) ───────────────────────
    IAudioOutput*     audioOutput     = nullptr;  // defaults to NullAudioOutput (low-level PCM)
    IAudioInput*      audioInput      = nullptr;
    IAudioModule*     audioModule     = nullptr;  // defaults to NullAudioModule (pipeline)
    ISynthEngine*     synthEngine     = nullptr;
    IVibration*       vibration       = nullptr;  // defaults to NullVibration
    IImu*             imu             = nullptr;  // defaults to NullImu
    IHttpClient*      httpClient      = nullptr;  // defaults to NullHttpClient
    ISettingsUI*      settingsUI      = nullptr;
    IKitManager*      kitManager      = nullptr;
    SequencerController* sequencer   = nullptr;
    ISequencerEngine* sequencerEngine = nullptr;
    IBleMidi*         bleMidi         = nullptr;  // defaults to NullBleMidi

    /// The varispeed sample engine, when the platform runs one. Portable apps
    /// reach it for what only the engine knows — its scope tap, its last note.
    /// Null on a platform without one; every user must check.
    PitchedInstrument* pitchedInstrument = nullptr;

    /// Decoder of the CrossPad SysEx catalog (STM32 frames and host frames
    /// alike). A portable app hangs its host-protocol callback here
    /// (setDawCallback). Null on a platform that has no such link; check.
    Stm32MessageHandler* stm32MessageHandler = nullptr;

    // ── Typed setters — auto-update PlatformCapabilities ────────────────

    /// Set MIDI output + toggle Capability::Midi.
    /// Special: also re-inits PadManager with the new output.
    void setMidiOutput(IMidiOutput* midi);

    /// Set audio output + toggle Capability::AudioOut.
    void setAudioOutput(IAudioOutput* audio);

    /// Set audio input + toggle Capability::AudioIn.
    void setAudioInput(IAudioInput* input);

    /// Set audio module (DSP pipeline) + toggle Capability::AudioOut.
    /// This is the canonical audio API; setAudioOutput is low-level PCM only.
    void setAudioModule(IAudioModule* module);

    /// Set synth engine + toggle Capability::Synth.
    void setSynthEngine(ISynthEngine* synth);

    /// Set vibration driver + toggle Capability::Vibration.
    void setVibration(IVibration* vib);

    /// Set IMU driver + toggle Capability::Imu.
    void setImu(IImu* imu);

    /// Set HTTP client + toggle Capability::Http.
    void setHttpClient(IHttpClient* http);

    /// Publish the pitched sample engine. No capability flag: the engine is a
    /// concrete object a port owns, not an interface a port may or may not have.
    void setPitchedInstrument(PitchedInstrument* instrument);

    /// Set BLE MIDI interface + toggle Capability::Bluetooth.
    /// Also wires into MidiInputHandler::setBleOutput().
    void setBleMidi(IBleMidi* ble);
};

/// Access the singleton PlatformServices registry.
PlatformServices& getPlatformServices();

/// Get the kit manager (may be nullptr if no kit system is available).
IKitManager* getKitManager();

} // namespace crosspad

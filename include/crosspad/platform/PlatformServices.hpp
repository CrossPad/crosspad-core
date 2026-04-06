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
class ISynthEngine;
class IKeyValueStore;
class IVibration;
class IImu;
class IHttpClient;
class ISettingsUI;
class IKitManager;
class IKitLoader;
class PadManager;
class PadLedController;
class PadAnimator;
class CrosspadSettings;
class CrosspadStatus;
class SequencerController;
class ISequencerEngine;

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
    IAudioOutput*     audioOutput     = nullptr;  // defaults to NullAudioOutput
    IAudioInput*      audioInput      = nullptr;
    ISynthEngine*     synthEngine     = nullptr;
    IVibration*       vibration       = nullptr;  // defaults to NullVibration
    IImu*             imu             = nullptr;  // defaults to NullImu
    IHttpClient*      httpClient      = nullptr;  // defaults to NullHttpClient
    ISettingsUI*      settingsUI      = nullptr;
    IKitManager*      kitManager      = nullptr;
    IKitLoader*       kitLoader       = nullptr;
    SequencerController* sequencer   = nullptr;
    ISequencerEngine* sequencerEngine = nullptr;

    // ── Typed setters — auto-update PlatformCapabilities ────────────────

    /// Set MIDI output + toggle Capability::Midi.
    /// Special: also re-inits PadManager with the new output.
    void setMidiOutput(IMidiOutput* midi);

    /// Set audio output + toggle Capability::AudioOut.
    void setAudioOutput(IAudioOutput* audio);

    /// Set audio input + toggle Capability::AudioIn.
    void setAudioInput(IAudioInput* input);

    /// Set synth engine + toggle Capability::Synth.
    void setSynthEngine(ISynthEngine* synth);

    /// Set vibration driver + toggle Capability::Vibration.
    void setVibration(IVibration* vib);

    /// Set IMU driver + toggle Capability::Imu.
    void setImu(IImu* imu);

    /// Set HTTP client + toggle Capability::Http.
    void setHttpClient(IHttpClient* http);
};

/// Access the singleton PlatformServices registry.
PlatformServices& getPlatformServices();

/// Get the kit manager (may be nullptr if no kit system is available).
IKitManager* getKitManager();

/// Get the kit loader (may be nullptr if no kit system is available).
IKitLoader* getKitLoader();

} // namespace crosspad

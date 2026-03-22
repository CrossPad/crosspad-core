// SPDX-License-Identifier: MIT
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/platform/PlatformCapabilities.hpp"

// Null stubs — default values for optional services
#include "crosspad/midi/NullMidiOutput.hpp"
#include "crosspad/synth/NullAudioOutput.hpp"
#include "crosspad/platform/NullVibration.hpp"
#include "crosspad/platform/NullImu.hpp"

// Interface headers for getter return types
#include "crosspad/event/IEventBus.hpp"
#include "crosspad/platform/IClock.hpp"
#include "crosspad/midi/IMidiOutput.hpp"
#include "crosspad/led/ILedStrip.hpp"
#include "crosspad/synth/IAudioOutput.hpp"
#include "crosspad/synth/IAudioInput.hpp"
#include "crosspad/synth/ISynthEngine.hpp"
#include "crosspad/settings/IKeyValueStore.hpp"
#include "crosspad/platform/IVibration.hpp"
#include "crosspad/platform/IImu.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/pad/PadLedController.hpp"
#include "crosspad/pad/PadAnimator.hpp"

namespace crosspad {

// ── Static null stubs (never deallocated) ───────────────────────────────

static NullMidiOutput   s_nullMidi;
static NullAudioOutput  s_nullAudio;
static NullVibration    s_nullVibration;
static NullImu          s_nullImu;

// ── Singleton registry ──────────────────────────────────────────────────

static PlatformServices& initServices() {
    static PlatformServices svc;
    // Non-null defaults for optional services
    svc.midiOutput = &s_nullMidi;
    svc.audioOutput = &s_nullAudio;
    svc.vibration = &s_nullVibration;
    svc.imu = &s_nullImu;
    return svc;
}

static PlatformServices& s_services = initServices();

PlatformServices& getPlatformServices() {
    return s_services;
}

// ── Typed setters with capability auto-tracking ─────────────────────────

void PlatformServices::setMidiOutput(IMidiOutput* midi) {
    midiOutput = midi ? midi : &s_nullMidi;

    if (midi) {
        addPlatformCapability(Capability::Midi);
    } else {
        removePlatformCapability(Capability::Midi);
    }

    // Re-init PadManager with the new MIDI output (mirrors crosspad_platform_set_midi)
    if (padManager && padLedController && padAnimator && eventBus && settings) {
        padManager->init(*padLedController, *padAnimator, *midiOutput,
                         *eventBus, settings, status);
        padManager->begin();
    }
}

void PlatformServices::setAudioOutput(IAudioOutput* audio) {
    audioOutput = audio ? audio : &s_nullAudio;

    if (audio) {
        addPlatformCapability(Capability::AudioOut);
    } else {
        removePlatformCapability(Capability::AudioOut);
    }
}

void PlatformServices::setAudioInput(IAudioInput* input) {
    audioInput = input;

    if (input) {
        addPlatformCapability(Capability::AudioIn);
    } else {
        removePlatformCapability(Capability::AudioIn);
    }
}

void PlatformServices::setSynthEngine(ISynthEngine* synth) {
    synthEngine = synth;

    if (synth) {
        addPlatformCapability(Capability::Synth);
    } else {
        removePlatformCapability(Capability::Synth);
    }
}

void PlatformServices::setVibration(IVibration* vib) {
    vibration = vib ? vib : &s_nullVibration;

    if (vib) {
        addPlatformCapability(Capability::Vibration);
    } else {
        removePlatformCapability(Capability::Vibration);
    }
}

void PlatformServices::setImu(IImu* newImu) {
    imu = newImu ? newImu : &s_nullImu;

    if (newImu) {
        addPlatformCapability(Capability::Imu);
    } else {
        removePlatformCapability(Capability::Imu);
    }
}

// ── Singleton getter functions ──────────────────────────────────────────
// Defined once here — platforms no longer need to implement these.

IEventBus& getEventBus() {
    return *s_services.eventBus;
}

IClock& getClock() {
    return *s_services.clock;
}

ILedStrip& getLedStrip() {
    return *s_services.ledStrip;
}

IMidiOutput& getMidiOutput() {
    return *s_services.midiOutput;
}

IAudioOutput& getAudioOutput() {
    return *s_services.audioOutput;
}

PadManager& getPadManager() {
    return *s_services.padManager;
}

PadLedController& getPadLedController() {
    return *s_services.padLedController;
}

PadAnimator& getPadAnimator() {
    return *s_services.padAnimator;
}

IVibration& getVibration() {
    return *s_services.vibration;
}

IImu& getImu() {
    return *s_services.imu;
}

} // namespace crosspad

[![API Docs](https://img.shields.io/badge/API_Docs-GitHub_Pages-blue?logo=github)](https://crosspad.github.io/crosspad-core/)

# crosspad-core

Portable C++ core library for CrossPad. Contains business logic, platform interfaces, and shared modules used across all CrossPad platforms via git submodule:

- [ESP32-S3 (Arduino)](https://github.com/CrossPad/ESP32-S3)
- [2playerCrosspad (ESP-IDF)](https://github.com/CrossPad/2playerCrosspad)
- [crosspad-pc (desktop simulator)](https://github.com/CrossPad/crosspad-pc)

Zero platform-specific code — every hardware dependency is behind a pure virtual interface.

## Architecture

```
include/crosspad/
  app/         App registry, lifecycle, flags, manager base
  audio/       Ring buffer, peak metering
  dsp/         FFT seam (portable + esp-dsp) and factory, pitch detection, rate conversion
  event/       Event bus interface and implementations
  instrument/  Pitched sample engine: WAV reader, sample bank, kit loader, voices
  kit/         Kit loading interface
  led/         LED strip interface, colors (RGB/HSL), animation types
  midi/        MIDI output interface, input handler
  music/       Music theory (scales, chords, progressions)
  pad/         PadManager, PadLedController, PadAnimator, pad logic
  platform/    Clock, capabilities, vibration, IMU interfaces
  protocol/    STM32 SysEx protocol, command sender, message handler
  sequencer/   Sequencer types and interfaces
  settings/    Key-value store, CrosspadSettings, settings UI callback
  status/      System status tracking
  stm32/       STM32 manager (keep-alive, polling)
  synth/       Synth engine, audio output/input interfaces
  core.h       Master include
```

## Platform Interfaces

Each platform provides concrete implementations of these interfaces:

| Interface | Purpose |
|-----------|---------|
| `IEventBus` | Thread-safe event dispatch (async post / sync send) |
| `IClock` | Microsecond time source |
| `ILedStrip` | RGB LED hardware control (16 pixels) |
| `IMidiOutput` | MIDI note, CC, pitch bend, aftertouch output |
| `IAudioOutput` | PCM audio output (write, sample rate, buffer size) |
| `IAudioInput` | PCM audio input capture |
| `ISynthEngine` | Polyphonic synth (note on/off, ADSR, filters, effects) |
| `IKeyValueStore` | Persistent key-value storage with namespaces |
| `IStm32Transport` | Byte-level STM32 co-processor communication |
| `IPadLogicHandler` | App-specific pad press/release behavior |
| `ISettingsUI` | Platform callback for settings save/exit/info |
| `IVibration` | Haptic feedback motor control |
| `IImu` | Accelerometer / IMU readings |

## Key Modules

### App System

- **AppRegistry** — singleton holding up to 16 registered apps
- **AppEntry** — metadata struct (name, icon, LVGL factory functions, callbacks, priority)
- **REGISTER_APP macro** — generates `_register_<Name>_app()` functions, auto-discovered by cmake or called explicitly
- **AppFlags** — MSGBOX
- **IApp** — lifecycle callbacks (onAppInit, onAppDeinit, onAppUpdate, onAppActivated)

### Pad System

- **PadManager** — central orchestrator for 16 pads: input handling, MIDI note mapping, LED coordination, pad logic routing
- **PadLedController** — per-pad LED state machine with velocity-based colors and feedback modes
- **PadAnimator** — LED animations (progress bar, boot, success, error, wave, spiral, breathe)

### Platform Initialization

```cpp
crosspad::CrosspadPlatformConfig config;
config.eventBus        = &myEventBus;
config.clock           = &myClock;
config.ledStrip        = &myLedStrip;
config.midiOutput      = &myMidi;
config.padManager      = &myPadManager;
config.padLedController = &myPadLedController;
config.padAnimator     = &myPadAnimator;
config.kvStore         = &myStore;      // optional
config.settings        = mySettings;    // optional

crosspad::crosspad_platform_init(config);
```

Init sequence: EventBus -> Settings persistence -> PadAnimator -> PadLedController -> PadManager -> begin().

### Platform Capabilities

Bitflag query system — platforms declare what they support, apps query at runtime:

```cpp
// Platform init:
crosspad::setPlatformCapabilities(
    Capability::Midi | Capability::Pads | Capability::Display);

// App runtime:
if (crosspad::hasCapability(crosspad::Capability::Synth)) { ... }
if (crosspad::hasAnyCapability(Capability::WiFi | Capability::Bluetooth)) { ... }
```

Flags: `Midi`, `AudioOut`, `AudioIn`, `Synth`, `Pads`, `Leds`, `Encoder`, `Display`, `Persistence`, `Vibration`, `WiFi`, `Bluetooth`, `Usb`, `Imu`, `Stm32`, `Sequencer`.

### Event System

Three implementations of `IEventBus`:
- **EspEventBus** — ESP-IDF event loop (ESP32 / Arduino-ESP32)
- **FreeRtosEventBus** — FreeRTOS queue-based (PC with FreeRTOS)
- **SyncEventBus** — synchronous immediate dispatch

Event types: NoteOn, NoteOff, PadPressed, PadReleased, AppChanged, TempoChanged, SamplePlayback, and more.

### Settings

- **CrosspadSettings** — singleton container (keypad, audio per-pad, vibration, routing, wireless)
- **IKeyValueStore** — abstract persistence (ESP32: NVS, PC: filesystem JSON)
- **ISettingsUI** — platform callback for shared Settings UI

### STM32 Protocol

- **Stm32CommandSender** — builds and sends SysEx commands (power off, brightness, routing, DFU)
- **Stm32MessageHandler** — parses incoming SysEx, dispatches to PadManager/callbacks
- **SysExProtocol.h** — framing constants and command definitions

## App Registration

```cpp
// In MyApp.cpp:
REGISTER_APP(MyApp, nullptr, "icon.png",
             MyApp_create, MyApp_destroy,
             nullptr, nullptr, nullptr, 0);
```

This generates `void _register_MyApp_app()`. Platform registry generators discover
and call these functions at initialization.

Use `REGISTER_APP` directly. Apps that declare framework-managed pad logic use
`REGISTER_APP_PL` with the pad-logic name as its final argument.

## Singleton Accessors

```cpp
crosspad::getEventBus()
crosspad::getPadManager()
crosspad::getPadLedController()
crosspad::getPadAnimator()
crosspad::getClock()
crosspad::getMidiOutput()
CrosspadSettings::getInstance()
```

## API Documentation

Generate browsable API docs with [Doxygen](https://www.doxygen.nl/):

```bash
doxygen
# open docs/doxygen/html/index.html
```

Dark theme with sidebar navigation and hyperlinked source.

## Usage

Include as a git submodule in your platform repo:

```bash
# ESP-IDF component
git submodule add <url> components/crosspad-core

# Arduino (PlatformIO)
git submodule add <url> lib/crosspad-core

# Desktop simulator (CMake)
git submodule add <url> crosspad-core
# Then GLOB_RECURSE crosspad-core/src/*.cpp in CMakeLists.txt
```

Platform-specific adapters (clock, LED, MIDI, audio, etc.) are implemented in each platform repo.

# crosspad-core

Portable C++17 core library for CrossPad. It holds the event bus, pads, MIDI, settings, the sequencer, kits, the sampler and the audio graph. It also holds the `I*` interfaces that each platform implements. The ESP-IDF firmware ([platform-idf](https://github.com/CrossPad/platform-idf), as `components/crosspad-core`) and the PC simulator ([crosspad-pc](https://github.com/CrossPad/crosspad-pc), as `lib/crosspad-core`) compile the same sources. `library.json` keeps it buildable as a PlatformIO library too, for the [Arduino firmware](https://crosspad.app/docs/legacy/esp32-s3-arduino/).

## Quickstart

A platform builds this library; there is nothing to run on its own. To run the host tests (C++17 compiler, CMake 3.14+, Ninja; on Windows `tests\build.bat`):

```bash
cd tests
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/crosspad_core_tests --reporter compact
```

## Documentation

- [Architecture](https://crosspad.app/docs/concepts/architecture/#crosspad-core-portable-logic), [Where code belongs](https://crosspad.app/docs/concepts/where-code-belongs/) and [Platform parity](https://crosspad.app/docs/concepts/platform-parity/) on https://crosspad.app/docs/.
- [Library reference](#library-reference) below; full API docs: run `doxygen` here and open `docs/doxygen/html/index.html`.
- Contributor notes (build systems, tests, versioning, releases): [CLAUDE.md](CLAUDE.md), [CHANGELOG.md](CHANGELOG.md).

## License

GPL-3.0-or-later — see [LICENSE](LICENSE).

### No warranty, intended use

CrossPad is a musical instrument. This software is provided "as is", without
warranty of any kind (GPL-3.0 sections 15 and 16). It is not designed, tested
or certified for safety-critical use — medical, military, aviation,
automotive, nuclear or life-support systems — and should not be relied on
where a failure could cause injury, death or damage to property. Details:
[Legal](https://crosspad.app/docs/reference/legal/).

## Library reference

The docs site explains the layers but has no API reference for the library, so this overview stays here. The headers are the source of truth.

### Layout

```
include/crosspad/
  app/          App registry, registrar macros, lifecycle, flags, app versions
  audio/        Audio modules and nodes, ring buffer, format conversion, peak meter, scope tap, WAV writer, output EQ
  config/       Board configuration (Configuration.h, boards), feature-flag schema
  control/      State behind the control verbs (pad input, kit/wave/pitched status, mixer levels) the board and the simulator both answer
  dsp/          FFT seam (portable; accelerated backend registered by the platform), pitch detection, rate conversion (whole or streaming), scope trigger, spectrum bars
  event/        Event bus interface, event types, implementations
  fw/           Firmware library and update logic (semver, catalog, store, schedule)
  imu/          IMU-to-MIDI mapping
  input/        Encoder acceleration
  instrument/   Pitched sample engine: WAV reader, sample bank, kit loader, voices
  kit/          Kit loading interface and JSON parser
  led/          LED strip interface, colors (RGB/HSL), animation types
  looper/       Looper engine
  midi/         MIDI output and BLE MIDI interfaces, input handler, MIDI clock
  music/        Music theory (scales, chords, progressions)
  net/          WiFi types, HTTP client interface, remote frames
  pad/          PadManager, PadLedController, PadAnimator, pad logic
  platform/     Clock, IMU, vibration, I2C device, capabilities, services, platform init
  protocol/     STM32 SysEx protocol, command senders, message handler
  sampler/      Streaming sampler engine
  sampletools/  Slice math, sample FX
  sequencer/    Sequencer types, engine interface, pattern sequencer, metronome
  settings/     Key-value store, CrosspadSettings, settings UI callback
  song/         Song engine, audio song, live set
  status/       System status tracking
  stm32/        STM32 manager, register bus, config sync, note map, velocity curve
  synth/        Synth engine, audio output/input interfaces
  transport/    Song transport (tempo, play/stop, locate)
  util/         Small shared helpers
  transport/    Song and sequencer transport
  util/         Small helpers
  core.h        Master include
```

### Platform interfaces

The main interfaces a platform implements (`grep -l 'virtual .* = 0' include/crosspad/*/I*.hpp` lists the rest):

| Interface | Purpose |
|-----------|---------|
| `IEventBus` | Thread-safe event dispatch (async post / sync send) |
| `IClock` | Microsecond time source |
| `ILedStrip` | RGB LED strip (pad LEDs) |
| `IMidiOutput` | MIDI note, CC, pitch bend, aftertouch output |
| `IBleMidi` | Bluetooth LE MIDI |
| `IAudioOutput` / `IAudioInput` | PCM audio output and capture |
| `ISynthEngine` | Polyphonic synth (note on/off, ADSR, filters, effects) |
| `IKeyValueStore` | Persistent key-value storage with namespaces |
| `IStm32Transport` | Byte-level STM32 co-processor communication |
| `IStm32RegisterBus` | Register-level I2C access to the STM32 (v2 boards) |
| `ISettingsUI` | Platform callback for settings save/exit/info |
| `IVibration` | Haptic feedback motor control |
| `IImu` | Accelerometer / IMU readings |
| `IHttpClient` | HTTP client (`Capability::Http`) |
| `ISongEnginePlatform` | Platform hooks for the song engine (worker thread, ring memory, project location, bounce bus) |
| `IInstrumentHost` | Lends the pitched sample engine to an app that plays an instrument |

Apps implement `IPadLogicHandler` for their own pad behaviour.

### Platform initialization

```cpp
crosspad::CrosspadPlatformConfig config;
config.eventBus         = &myEventBus;
config.clock            = &myClock;
config.ledStrip         = &myLedStrip;
config.midiOutput       = &myMidi;
config.padManager       = &myPadManager;
config.padLedController = &myPadLedController;
config.padAnimator      = &myPadAnimator;
config.kvStore          = &myStore;      // optional
config.settings         = mySettings;    // optional

crosspad::crosspad_platform_init(config);
```

Init sequence: EventBus -> Settings persistence -> PadAnimator -> PadLedController -> PadManager -> begin().

### Platform capabilities

Platforms declare what they support; apps query it at runtime:

```cpp
// Platform init:
crosspad::setPlatformCapabilities(
    Capability::Midi | Capability::Pads | Capability::Display);

// App runtime:
if (crosspad::hasCapability(crosspad::Capability::Synth)) { ... }
if (crosspad::hasAnyCapability(Capability::WiFi | Capability::Bluetooth)) { ... }
```

Flags: `Midi`, `AudioOut`, `AudioIn`, `Synth`, `Pads`, `Leds`, `Encoder`, `Display`, `Persistence`, `Vibration`, `WiFi`, `Bluetooth`, `Usb`, `Imu`, `Stm32`, `Sequencer`, `Http`.

### App registration

```cpp
// In MyApp.cpp:
REGISTER_APP(MyApp, nullptr, "icon.png",
             MyApp_create, MyApp_destroy,
             nullptr, nullptr, nullptr, 0);
```

This generates `void _register_MyApp_app()`. Platform registry generators discover
and call these functions at initialization. Apps that declare framework-managed pad
logic use `REGISTER_APP_PL` with the pad-logic name as its final argument. The
registry holds up to 24 apps. Writing an app: [New app](https://crosspad.app/docs/how-to/new-app/).

### Singleton accessors

```cpp
crosspad::getEventBus()
crosspad::getPadManager()
crosspad::getPadLedController()
crosspad::getPadAnimator()
crosspad::getClock()
crosspad::getMidiOutput()
CrosspadSettings::getInstance()
```

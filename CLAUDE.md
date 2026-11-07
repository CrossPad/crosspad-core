# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

**crosspad-core** is the portable C++17 library for the CrossPad hardware — a musical MIDI controller/sampler/sequencer with 16 pressure-sensitive pads, RGB LEDs, rotary encoder, and an STM32 co-processor. It holds the interfaces and the logic; platform-specific implementations live in the consumer projects. No ESP-IDF or FreeRTOS code is reachable from core: `EspEventBus`, `EspDspFft`, the DRV2605 driver and `FreeRtosEventBus` live in the platforms, and an accelerated FFT or render kernel is registered at runtime (`setFftBackend()`, `dsp::setAudioKernels()`).

User-facing docs live on https://crosspad.app/docs/: [Architecture](https://crosspad.app/docs/concepts/architecture/), [Where code belongs](https://crosspad.app/docs/concepts/where-code-belongs/), [Platform parity](https://crosspad.app/docs/concepts/platform-parity/), [Manifesto](https://crosspad.app/docs/concepts/manifesto/). The README keeps the library reference (layout, interfaces, platform init, capabilities, app registration). A change that breaks a docs page (command, output, path) opens an issue in CrossPad/crosspad-docs.

## Consumers and Build Systems

This library is consumed three ways:

**ESP-IDF component** — [platform-idf](https://github.com/CrossPad/platform-idf), as a git submodule at `components/crosspad-core/`:
- `CMakeLists.txt` uses `GLOB_RECURSE` on `src/*.cpp`, so new `.cpp` files are picked up automatically
- `REQUIRES` ArduinoJson only; the same `CMakeLists.txt` is the plain CMake build below

**Plain CMake** — top-level or `add_subdirectory()`: the static library target `crosspad-core` (`-DCROSSPAD_CORE_USE_LVGL=ON` when the consumer builds with `USE_LVGL`); `tests/` links it. [crosspad-pc](https://github.com/CrossPad/crosspad-pc) (submodule at `lib/crosspad-core/`) still globs `lib/crosspad-core/src/*.cpp` itself, filters out `SyncEventBus.cpp` and `Stm32Manager.cpp`, and brings its own `FreeRtosEventBus` (`src/pc_stubs/`)

**PlatformIO** (`library.json`, frameworks `arduino` + `espidf`) — the Arduino firmware ([platform-arduino](https://crosspad.app/docs/legacy/esp32-s3-arduino/), the legacy ESP32-S3 project):
- `library.json` controls which `.cpp` files are compiled via `srcFilter` (not all sources are built by default)
- When adding a new `.cpp` file, you **must** add it to `library.json`'s `srcFilter` list

## Tests

Standalone Catch2 v3.5.0 suite in `tests/` (links the `crosspad-core` target, `SyncEventBus`, ArduinoJson via FetchContent; no FreeRTOS, ESP-IDF or SDL2):

```bash
cd tests
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/crosspad_core_tests --reporter compact      # or a tag filter, e.g. "[audio]"
```

Windows: `tests\build.bat` (VS 2022 vcvarsall + VS cmake). `tests/test_*.cpp` files are globbed and link the whole library (`add_subdirectory` of the repo root), so a new `.cpp` is under test with no list to edit. CI (`.github/workflows/audio-tests.yml`) runs the same commands in the `crosspad-ci` image, but only when `include/crosspad/audio/**`, `include/crosspad/synth/**`, `AudioEngineSettings.hpp`, `tests/**` or the workflow change — run the suite yourself for other changes. There is no lint step.

## Versioning and Releases

- The version is the triple in `include/crosspad/CrosspadCoreVersion.hpp` (bump rules in its header comment: MAJOR when an interface a consumer implements or calls changes shape, MINOR when something is added, PATCH for fixes). `library.json` `version` carries the same number — bump both in one commit, and add a `CHANGELOG.md` entry.
- Consumers gate on it at compile time with `crosspad/RequireVersion.hpp`: a major mismatch is an `#error`, minor/patch drift a `#warning` (crosspad-pc: `src/crosspad_deps.hpp`; crosspad-gui: `CROSSPAD_GUI_REQUIRES_CORE_*`).
- `.github/workflows/lib-release.yml`: a push to `main` that touches `library.json` or `CrosspadCoreVersion.hpp` tags `vX.Y.Z` through CrossPad/.github's reusable `app-release.yml` in library mode (a bare tag, no GitHub Release). platform-idf's hourly bump reads the tags and moves core when an app needs it.
- `.github/workflows/notify-downstream.yml`: a `v*` tag or a merged PR sends a `submodule-update` dispatch to crosspad-pc, ESP32-S3 and platform-idf.
- `.github/workflows/docs.yml` builds the Doxygen HTML on `main` as a workflow artifact (`api-docs`); nothing publishes it to GitHub Pages.

## Three-Layer Architecture

Layer 1 is crosspad-core (this repo), layer 2 crosspad-gui (LVGL UI, `AppBase<Derived>` CRTP), layer 3 the platforms (platform-idf `components/crosspad-platform-idf/`, crosspad-pc `src/pc_stubs/` and friends, the Arduino firmware). The diagram and the rules are on [Architecture](https://crosspad.app/docs/concepts/architecture/) and [Where code belongs](https://crosspad.app/docs/concepts/where-code-belongs/).

## Core Design Pattern: Interface-Based Abstraction

Every hardware/platform dependency is behind a pure virtual interface (`I` prefix). Consumer projects provide implementations — `Esp*` adapters in platform-idf, `Pc*` in crosspad-pc, `Arduino*` in the Arduino firmware:

| Interface | Purpose | platform-idf | crosspad-pc |
|-----------|---------|--------------|-------------|
| `IEventBus` | Async event dispatch | `EspEventBus` (esp_event, in crosspad-platform-idf) | `FreeRtosEventBus` (`src/pc_stubs/`) |
| `ILedStrip` | LED hardware control | `EspLedStrip` | `PcLedStrip` |
| `IKeyValueStore` | Persistent settings | `NvsSettingsStore` | `PcKeyValueStore` (JSON file) |
| `IStm32Transport` | UART to STM32 | `EspStm32Transport` | — (no co-processor) |
| `IClock` | Timestamps | `EspClock` | `PcClock` |
| `IMidiOutput` | MIDI send | `UsbMidiOutput`, `Stm32MidiOutput` (`main/midiCallbacks.cpp`) | `PcMidi` (RtMidi) |
| `IBleMidi` | BLE MIDI | `EspBleMidi` | `PcBleMidi` (SimpleBLE) |
| `ISynthEngine` | Audio synthesis | — | `MlPianoSynth` |
| `IPadLogicHandler` | Per-app pad behavior | App-specific handlers | App-specific handlers |

`ISequencerEngine` has no implementation in either platform today; the Arduino firmware's `Arduino*` adapters (`ArduinoClock`, `ArduinoStm32Transport`, `ArduinoSequencerEngine`, …) include one. Ask `crosspad_interface_implementations` (MCP) for the current list.

Global singletons are accessed via free functions: `crosspad::getEventBus()`, `crosspad::getPadManager()`, `CrosspadSettings::getInstance()`.

## Key Subsystems

### Event System (`event/`)
- Pub/sub with `EventType` enum: NoteOn, NoteOff, PadPressed, PadReleased, AppChanged, TempoChanged, etc.
- `EventSource` tracks origin (PadManager, MidiInput, App, SequencerCallback, etc.) to prevent feedback loops
- Post = async (queued), Send = sync (ISR-safe)
- `IEventBus` is implemented by each platform (`EspEventBus` in crosspad-platform-idf, `FreeRtosEventBus` in crosspad-pc); core ships `SyncEventBus` (synchronous immediate dispatch; the host tests)

### App System (`app/`)
- `IApp`: base interface with lifecycle hooks (`onAppInit`, `onAppDeinit`, `onAppUpdate`, `onAppActivated`)
- `AppRegistry`: singleton holding up to 24 (`MAX_APPS`) registered `AppEntry` descriptors
- `AppEntry`: metadata struct (name, icon, LVGL factory functions, callbacks, priority)
- `AppFlags`: `APP_FLAG_MSGBOX`
- `AppRegistrar.hpp`: named `REGISTER_APP` / `REGISTER_APP_PL` registration macros

### Pad System (`pad/`)
- `PadManager`: central orchestrator for 16 pads — handles press/release, MIDI mapping, chord notes, delegates to `PadLedController` and `PadAnimator`
- `IPadLogicHandler`: apps register custom pad behavior; `PadManager` swaps active handler on app switch
- `PadLedController`: per-pad color state machine (default/pressed/playing colors, velocity-based colors, feedback modes, brightness)
- `PadAnimator`: LED animations (progress bar, boot, success, error, charging, still frames) using `ILedStrip` + `IClock`. One runs at a time and every start takes the pads; each start returns an `AnimToken` -- `owns(token)`, `updateFrame(token, …)` and `stop(token)` act only on the caller's own animation, `stopAll()` is for system paths (soft-off, sleep)

### STM32 Protocol (`protocol/`, `stm32/`)
- SysEx framing over UART (manufacturer ID 0x7D, `SysExProtocol.h`); on v2 boards also the I2C register map (`stm32/reg_map.h`, `IStm32RegisterBus`)
- `Stm32CommandSender`: command interface (power off, brightness, routing matrix, keep-alive, DFU); `SysExStm32CommandSender` and `I2cStm32CommandSender` implement it
- `Stm32MessageHandler`: parses incoming messages (pad events, encoder, buttons, battery status) and dispatches to `PadManager` and callbacks
- `Stm32Manager`: maintenance pass (keep-alive, routing updates, amp gate) the platform runs every `kTickPeriodMs` from its own task

### Settings (`settings/`)
- `CrosspadSettings` singleton: keypad, routing (STM/ESP), wireless, audio (per-pad × 16 + preview + master FX), audio engine, vibration, IMU MIDI, STM config, theme colour, LCD and RGB brightness
- Load/save via `IKeyValueStore` with namespace scoping (NVS on the board, a JSON file in the simulator)
- `ISettingsUI`: platform callback for the shared settings screen (crosspad-gui)

### Music Theory (`music/`)
- Static scale/chord/progression definitions
- `MusicTheory::rebuildPadMap()` computes MIDI note assignments for 16 pads based on root/octave/scale/chord settings

## File Organization Convention

```
include/crosspad/
  {subsystem}/
    I{Name}.hpp      — Pure virtual interface
    {Name}.hpp       — Concrete class or data structures
    {Name}Types.hpp  — Enums and type definitions
src/
  {subsystem}/
    {Name}.cpp       — Implementation
```

Headers use `#pragma once`. C-compatible headers (like `SysExProtocol.h`) use `.h` extension with `extern "C"` guards.

## Consumer Integration Pattern

Platform projects initialize crosspad-core via a platform init function that:
1. Creates its `IEventBus` (ESP-IDF: `EspEventBus` in crosspad-platform-idf; PC: `FreeRtosEventBus`; static, with task params)
2. Creates platform clock, MIDI output, LED strip implementations
3. Constructs `PadLedController`, `PadAnimator` and `PadManager`
4. Fills `CrosspadPlatformConfig` and calls `crosspad_platform_init()`, which wires them (order in the README § Platform initialization)
5. Apps register via named functions generated by `REGISTER_APP` and invoked by the platform registry generator
6. Apps register `IPadLogicHandler` instances with `PadManager::registerPadLogic()`

## Dependencies

- `ArduinoJson` (bblanchon) — settings serialization
- `espressif__esp-dsp` — on esp32s3 only (`dsp/EspDspFft.cpp`, the SIMD pan accumulate in `PitchedInstrument`)
- Host tests: Catch2 v3.5.0 and ArduinoJson v7.3.0 via FetchContent
- C++17 standard library (`<functional>`, `<mutex>`, `<map>`, `<vector>`, `<string>`)

## Important Conventions

- Platform variance goes through interfaces and runtime registration. The few exceptions are guarded and should not grow: `AlignedVector` (`ESP_PLATFORM`: internal/PSRAM caps), `_WIN32` paths in `PosixFileSystem.cpp`
- `Pad` struct uses fixed arrays of 16 elements (pads are 0-indexed)
- MIDI notes are `uint8_t`, velocities are `uint8_t`, channels are `uint8_t`
- Color system: `RgbColor` (0-255 per channel) with HSL conversion support
- `AppEntry` has both LVGL and simple GUI callback slots — not mutually exclusive but typically one path is used
- When `USE_LVGL` is defined by consumer, `AppEntry.hpp` conditionally includes LVGL types

## CrossPad Manifesto

The principles behind the layering (write once, run everywhere; thin platform repos; open by default; documentation ships with the code) are on https://crosspad.app/docs/concepts/manifesto/.

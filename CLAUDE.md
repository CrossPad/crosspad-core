# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

**crosspad-core** is a portable C++ library for the CrossPad hardware — a musical MIDI controller/sampler/sequencer with 16 pressure-sensitive pads, RGB LEDs, rotary encoder, and an STM32 co-processor. This repo contains **only platform-agnostic interfaces and logic**; platform-specific implementations live in consumer projects.

## Build Systems

This library is consumed two ways:

**PlatformIO** (Arduino framework — ESP32-S3 project):
- `library.json` controls which `.cpp` files are compiled via `srcFilter` (not all sources are built by default)
- When adding a new `.cpp` file, you **must** add it to `library.json`'s `srcFilter` list

**ESP-IDF component** (pure ESP-IDF projects like P4):
- `CMakeLists.txt` uses `GLOB_RECURSE` so new `.cpp` files are picked up automatically
- Used as a git submodule under `components/crosspad-core/`

There are no test or lint commands in this repo — it's a library compiled as part of consumer projects.

## Three-Layer Architecture

The CrossPad ecosystem has three layers. This repo is Layer 1:

```
Layer 1: crosspad-core (this repo)
  → Portable C++ interfaces + business logic, zero platform deps

Layer 2: crosspad-gui
  → Shared LVGL UI components, depends on crosspad-core
  → CRTP template AppBase<Derived> for LVGL app lifecycle

Layer 3: Platform projects (ESP32-S3, P4_TEST/2playerCrosspad)
  → Concrete implementations of all interfaces
  → Arduino: lib/crosspad_lib/src/platform/ (ArduinoCrosspadPlatform)
  → ESP-IDF: components/crosspad-platform-idf/
```

## Core Design Pattern: Interface-Based Abstraction

Every hardware/platform dependency is behind a pure virtual interface (`I` prefix). Consumer projects provide implementations:

| Interface | Purpose | ESP32-S3 Implementation |
|-----------|---------|------------------------|
| `IEventBus` | Async event dispatch | `EspEventBus` (esp_event) |
| `ILedStrip` | LED hardware control | `NeoPixelLedStrip` (NeoPixelBus) |
| `IKeyValueStore` | Persistent settings | Arduino Preferences / NVS |
| `IStm32Transport` | UART to STM32 | `ArduinoStm32Transport` (Serial1) |
| `IClock` | Timestamps | `ArduinoClock` (millis()) |
| `IMidiOutput` | MIDI send | `ArduinoMidiOutput` |
| `ISequencerEngine` | Sequencer backend | `ArduinoSequencerEngine` (ML_MidiSeq) |
| `ISynthEngine` | Audio synthesis | Platform synth wrapper |
| `IPadLogicHandler` | Per-app pad behavior | App-specific handlers |

Global singletons are accessed via free functions: `crosspad::getEventBus()`, `crosspad::getPadManager()`, `CrosspadSettings::getInstance()`.

## Key Subsystems

### Event System (`event/`)
- Pub/sub with `EventType` enum: NoteOn, NoteOff, PadPressed, PadReleased, AppChanged, TempoChanged, etc.
- `EventSource` tracks origin (PadManager, MidiInput, App, SequencerCallback, etc.) to prevent feedback loops
- Post = async (queued), Send = sync (ISR-safe)

### App System (`app/`)
- `IApp`: base interface with lifecycle hooks (`onAppInit`, `onAppDeinit`, `onAppUpdate`, `onAppActivated`)
- `AppRegistry`: singleton holding up to 16 registered `AppEntry` descriptors
- `AppRegistrar.hpp`: named `REGISTER_APP` / `REGISTER_APP_PL` registration macros

### Pad System (`pad/`)
- `PadManager`: central orchestrator for 16 pads — handles press/release, MIDI mapping, chord notes, delegates to `PadLedController` and `PadAnimator`
- `IPadLogicHandler`: apps register custom pad behavior; `PadManager` swaps active handler on app switch
- `PadLedController`: per-pad color state machine (default/pressed/playing colors, brightness)
- `PadAnimator`: LED animations (progress bar, boot, success, error) using `ILedStrip` + `IClock`

### STM32 Protocol (`protocol/`, `stm32/`)
- SysEx framing over UART (manufacturer ID 0x7D)
- `SysExFramer`: state machine assembling frames → callback dispatch
- `Stm32CommandSender`: builds commands (power off, brightness, routing matrix, keep-alive, DFU)
- `Stm32MessageHandler`: parses incoming messages (pad events, encoder, battery status)
- `Stm32Manager`: FreeRTOS maintenance task (keep-alive, routing updates)

### Settings (`settings/`)
- `CrosspadSettings` singleton: keypad, routing (STM/ESP), wireless, audio (per-pad × 16 + preview + master FX), vibration, UI brightness
- Load/save via `IKeyValueStore` with namespace scoping

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
1. Creates `EspEventBus` (static, with task params)
2. Creates platform clock, MIDI output, LED strip implementations
3. Constructs `PadLedController` and `PadAnimator`
4. Constructs `PadManager` wired to all of the above
5. Apps register via named functions generated by `REGISTER_APP` and invoked by the platform registry generator
6. Apps register `IPadLogicHandler` instances with `PadManager::registerPadLogic()`

## Dependencies

- `ArduinoJson` (bblanchon) — settings serialization
- `freertos` — task management (FreeRTOS types used in headers)
- `esp_event` — EspEventBus implementation
- C++17 standard library (`<functional>`, `<mutex>`, `<map>`, `<vector>`, `<string>`)

## Important Conventions

- No `#ifdef PLATFORM` in this repo — all platform variance goes through interfaces
- `Pad` struct uses fixed arrays of 16 elements (pads are 0-indexed)
- MIDI notes are `uint8_t`, velocities are `uint8_t`, channels are `uint8_t`
- Color system: `RgbColor` (0-255 per channel) with HSL conversion support
- `AppEntry` has both LVGL and simple GUI callback slots — not mutually exclusive but typically one path is used
- When `USE_LVGL` is defined by consumer, `AppEntry.hpp` conditionally includes LVGL types

## CrossPad Manifesto

### Write once, run everywhere
CrossPad runs the same core logic on ESP32-S3 hardware and desktop simulators. Platform differences are hidden behind thin abstraction layers — not thick frameworks. Business logic should never know what chip it's running on.

### Platform repos are thin, shared repos are thick
Cross-platform is a first-class goal. Platform-specific repositories (ESP32-S3, 2playerCrosspad, crosspad-pc) should contain only what cannot be shared — hardware drivers, HAL bindings, build system glue. All business logic belongs in crosspad-core; all UI components belong in crosspad-gui. If you're writing code that could work on another platform, it doesn't belong in a platform repo.

### Hardware is software you can touch
We design hardware and software as one system. The pad grid, the encoder, the LED strip — they are first-class citizens with well-defined interfaces. If you can simulate it on a PC, you can ship it on a board.

### Small team, big surface area
We are a small team. This is a feature, not a limitation. Every architectural decision is made to reduce maintenance burden: shared init sequences, unified event buses, portable abstractions. Code that exists in two places will eventually exist in one.

### Open by default
Schematics, firmware, PC tools, documentation — all open source. Not because it's trendy, but because a music controller you can't modify isn't yours. We want people to fork, adapt, and build things we never imagined.

### Community-driven, not committee-driven
We welcome contributions, but we ship fast. A clear architecture and good documentation lower the barrier to entry. You shouldn't need to read the entire codebase to add an app or write a new platform driver.

### Documentation is not an afterthought
If it's not documented, it doesn't exist. API references, architecture guides, build instructions — they ship with the code, not after it.

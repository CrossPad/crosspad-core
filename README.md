[![API Docs](https://img.shields.io/badge/API_Docs-GitHub_Pages-blue?logo=github)](https://crosspad.github.io/crosspad-core/)

# crosspad-core

Portable C++ core library for CrossPad. Contains business logic and interfaces shared across [ESP-IDF](https://github.com/CrossPad/2playerCrosspad) and [Arduino](https://github.com/CrossPad/ESP32-S3) platforms via git submodule.

## Key Interfaces

| Interface | Purpose |
|-----------|---------|
| `IEventBus` | Thread-safe event dispatch |
| `ILedStrip` | RGB LED control |
| `IClock` | Time abstraction |
| `IMidiOutput` | MIDI output |
| `IStm32Transport` | STM32 co-processor communication |
| `IKeyValueStore` | Persistent settings |
| `IPadLogicHandler` | Per-app pad behavior |

## Key Modules

- **AppRegistry** / `REGISTER_APP` macro — app registration and discovery
- **PadManager**, **PadLedController**, **PadAnimator** — pad input, LED colors, animations
- **Stm32Manager**, **Stm32CommandSender** — STM32 co-processor protocol
- **EspEventBus** — ESP-IDF event loop (works on both ESP-IDF and Arduino-ESP32)
- **CrosspadSettings** — persistent configuration

## API Documentation

Generate browsable API docs with [Doxygen](https://www.doxygen.nl/):

```bash
doxygen
# open docs/doxygen/html/index.html
```

Dark theme with sidebar navigation and hyperlinked source.

## Usage

This library is included as a git submodule in platform repos:

```bash
# ESP-IDF
git submodule add <url> components/crosspad-core

# Arduino (PlatformIO)
git submodule add <url> lib/crosspad-core
```

Platform-specific adapters (clock, LED, MIDI, etc.) are implemented in each platform repo — crosspad-core contains zero platform-specific code.

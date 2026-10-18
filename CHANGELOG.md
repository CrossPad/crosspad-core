# Changelog

Notable changes to crosspad-core. The version is the triple in
`include/crosspad/CrosspadCoreVersion.hpp` (bump rules there; `library.json`
carries the same number) and consumers gate on it with
`crosspad/RequireVersion.hpp`. This file starts at 1.45.0; earlier releases
are described in their `chore: X.Y.Z` commits.

## 2.1.0 — 2026-10-19

The 1.x line's 1.71.0 on top of 2.0.0.

### Added

- A Loop pad is a launch/mute toggle (platform-idf#82): a hit starts it, on
  the next bar line while the clock runs (to the frame, in the render) and at
  once while it is stopped; a hit while it sounds mutes it with the clock
  running -- the voice keeps advancing and streaming -- or stops it with the
  clock stopped; a hit while muted unmutes it at its current phase; a hit on
  a launch still waiting cancels it; the clock stopping ends every loop.
  `SamplerEngine::setBarClock()`, `loopState()`, `Params::muteMs`, counters
  `loopsQueued`/`loopsLaunched`; `MetronomeNode::framesToBar()`;
  `PadLedController::notifyNoteDimmed()` for a muted or waiting loop.
  The dimmed colour keeps the playing colour's hue at half its brightness,
  its brightest channel between an eighth and a quarter of full scale.
- `transportSetHome()` / `transportGoHome()`: a home target the transport
  returns to when the holder that claimed it is left stopped (the pattern on
  the board and the simulator), so the top bar's REC/PLAY never stay greyed
  after an app that claimed them.

### Changed

- `KitInfo::pads` is a lazy `KitPads`: a listed kit costs a pointer, not
  `KIT_PADS` pads (1176 -> ~220 B a kit); the block, once there, keeps its
  address for the kit's life.

### Fixed

- `PortableKitLoader::scanKits()` lists every kit on the card: it counts them
  first and reserves the list once, at the count plus `kSpareKits`. Grown by
  doubling from a guess, a card of 88 kits needed a 64 kB and a 128 kB block
  at once; on a boot with the update check on WiFi that copy threw and the
  list stopped at 64 kits. Short of one block that big, the spare goes first,
  then whole kits, and the scan says how many it left out. `addKit()` takes
  the slot of a kit removed since the scan once the spare room is used up.
- The first take survives its STOP: the song opens at start, and the scenes
  store keeps a take played before a song was open. A song with nothing in
  it takes the loaded kit.
- The pad input tap skips `EventSource::App`: an app playing the pads itself
  (a kit preview) is not recorded into a take.
- Sample tools: a take borrows lent arena memory and a reclaim ends it written
  out; no new take while a cut one is closing; a take that got nothing leaves
  no file.

## 2.0.0 — 2026-10-18

The open-source baseline: core holds no ESP-IDF, FreeRTOS or board-revision
code any more, and builds as a plain CMake library. Contains every release
from 1.54.1 to 1.70.0 below (1.56.0..1.62.0 and 1.70.0 are described in their
commits).

### Removed

- `EspEventBus`, `Drv2605Driver` and `EspDspFft` (now in
  crosspad-platform-idf) and `FreeRtosEventBus` (now in crosspad-pc). Core
  keeps the interfaces (`IEventBus`, `IVibration`, `IFft`) and `SyncEventBus`.
- Board profiles: `crosspad/config/boards.h`, `crosspad/config/board/*.h`,
  `CROSSPAD_BOARD`, `BOARD_CROSSPAD_V1`/`V2`/`PC` and the `board` group of
  `features.schema.json`. The `BOARD_*_DEF` defaults keep their names and
  values in `Configuration_adv.h`, each overridable with `-D`.
- `CROSSPAD_HAS_ESP_DSP`, `CROSSPAD_HAS_ESP_DSP_FFT` and the PUBLIC
  `CROSSPAD_USE_ESP_DSP=1` and `CROSSPAD_SIMD_PIE=1` definitions; a consumer
  that uses esp-dsp itself defines its own. The ESP-IDF component requires
  only ArduinoJson.
- `CROSSPAD_DSP_PIE` and `dsp::pie::*` (the S3's EE.LDF.128 / EE.STF.128
  loops) and the TRUNC.S / CLAMPS steps of `dsp::floatToInt16()`: the
  ESP-IDF platform registers them with `dsp::setAudioKernels()`.
- `crosspad/fw/FwCardActions.hpp`: the kit card's action table is in
  crosspad-gui (`crosspad-gui/fw/FwCardActions.hpp`).
- `Stm32Manager::checkDfuMode()`, `setResetCallback()`,
  `setDfuButtonCheckCallback()` and their typedefs.
- `PadAnimator::stop()` without a token (use `stopAll()`).

### Changed
- `CrosspadSettings::launcherStyle` defaults to `LauncherStyle::List` (was Grid), in the constructor and in `load()`.

- `dsp::mac()`, `macPanned()`, `scaleStereo()`, `peakStereo()`,
  `clampPeakStereo()`, `biquadStereo()` and `floatToInt16()` keep their
  names and results; each runs the kernel the platform registered, or the
  portable one (`dsp::fast::*`).
- `WavLayout::parse()` without a chunk list stops once it has the data;
  the results are the same. `WavLayout::fileBytes` is 0 after an unclamped
  `wavReadLayout()`.
- `control::readMixerLevels()` takes the mixer non-const (the limiter
  readings are taken) and needs its `getChannelTrim()`,
  `getOutputCeiling()`, `takeOutputLimit()` and `kTapLimit`.
- `IKitConvertHost::makeDir()` defaults to `IFileSystem::makeDir()`, and
  `KitRateConverter` lists, stats and removes folders through
  `getFileSystem()`.
- `SampleToolsEngine`: a take with the song while the song already plays
  goes in at its next bar line instead of restarting it
  (`RecStatus::fromBar` is that bar).
- `Stm32Manager::init()` takes an `IClock&` instead of a `CrosspadStatus*`.
  `start()`/`stop()` no longer create or end a task: the platform calls
  `tick()` every `Stm32Manager::kTickPeriodMs`.
- `PadAnimator`: every starter (`startProgress()`, `showFrame()`,
  `playSuccess()`, `playError()`, `playBootAnim()`, `startCharging()`)
  returns an `AnimToken`; `stop(AnimToken)` stops only that animation and
  returns `bool`. `showFrame()` always starts a new frame; repaint a running
  one with `updateFrame(token, colors)`.
- `IPadLogicHandler::onPadPan()` (and `PadManager::notifyPadPan()`) carry
  -64..63, as every producer always sent, documented as -127..127 before.
- `PadManager`: once `onDeactivate()` returns, no pad-logic callback is in
  flight. Changing the active logic may therefore block for the length of a
  callback on another thread; do not change it from inside a pad callback.
- `IEventBus::unsubscribe()`: after it returns the handler is not running
  and is never called again -- except a handler unsubscribing itself from
  inside its call, which completes. Handlers may subscribe, unsubscribe and
  post on the bus that called them.
- An app's `destroyLVGL` may be called with `nullptr` when its root is
  already gone.

### Added

- A plain CMake target `crosspad-core` from the same `CMakeLists.txt` as the
  ESP-IDF component; `CROSSPAD_CORE_USE_LVGL` sets `USE_LVGL` for core and
  its consumers alike.
- `cmake/crosspad_app_registry.cmake` (`crosspad_generate_app_registry()`,
  the `REGISTER_APP` scan) and `cmake/crosspad_git_rev.cmake`
  (`crosspad_git_rev()`), shared by the CMake platforms.
- `FftBackend` and `setFftBackend()`: a platform registers its accelerated
  FFT; `createFft()` falls back to `PortableFft`.
- `dsp::AudioKernelTable` and `dsp::setAudioKernels()`: a platform registers
  its render kernels; `dsp::fast::floatToInt16With()`, `fast::truncSat()`
  and `fast::clampS16()`, the int16 loop a platform instantiates with its
  own steps.
- `SongEngineSizes` and `ISongEnginePlatform::sizes()`: ring and card-access
  sizes come from the platform.
- `SampleToolsEngine` behind `ISampleToolsPlatform`: the Recorder's and
  Slicer's engine, portable, with `requestNormalized()`.
- `IFileSystem` (list, stat, make/remove directories, rename, unbuffered
  reads), `PosixFileSystem` (the default), `getFileSystem()` and
  `PlatformServices::fileSystem`; it replaces crosspad-gui's own.
- `SubscriptionTable`: a subscriber list with the `unsubscribe()` contract,
  for event-bus implementations.
- `InfoRowTable`: the Settings -> Info rows.
- `dsp/SpectrumBars.hpp`: a spectrum analyser's Hann window, log-frequency
  bar mapping and dBFS levels (`dsp::spectrumHann()`, `spectrumMapBars()`,
  `spectrumBarHz()`, `spectrumLevels()`), from crosspad-gui's scope view.
- `wavReadLayout()` and `WavLayout::blockAlign`: one RIFF walk for every
  WAV reader -- the engine's rules in `WavLayout::parse()`, the lenient
  ones (either chunk order, declared sizes unless clamped) here.
- `FwDownloader` (with `IHttpStream`, `ISha256`, `FwBlocklist`): the
  resumable, signature- and SHA-256-checked package download.
- `crosspad/fw/FwPackage.hpp`: `fwPackageName()`, `FwPkgVerdict`,
  `FwSlotState`, `FwSlotInfo`; the kit icon format constants
  (`kFwIconSide`, `kFwIconBytes`, ...) in `FwCatalog.hpp`.
- `crosspad/control/*`: the device-control verbs read into plain structs
  once for every platform -- `KitStatus`, `KitList`, `LedState`,
  `MixerLevels` (with `MixerTapLevel`, each channel's `trim`, each output's
  `limit` and `ceiling`), `PitchedStatus`, `WaveStatus`, `PadInput`,
  `AudioLevel`, `AppName`.
- `IPadLogicHandler::kPanMin`, `kPanCentre`, `kPanMax`, `kPanWireCentre`
  and `panFromWire()`.
- `PadAnimator::AnimToken`, `kNoAnim`, `owns()`, `updateFrame()`,
  `stopAll()`; `Stm32Manager::tick()` and `kTickPeriodMs`.

### Fixed

- `SyncEventBus` called handlers with its mutex held: a handler that
  subscribed, unsubscribed or posted on the same bus deadlocked.
- A pad press or touch racing an app's teardown could reach the handler
  after `onDeactivate()`, starting or arming a note nothing would release.
- `MetronomeNode`: during the block that took a transport move,
  `positionSamples()` and `countInSamples()` still returned the old position.

## 1.69.0 — 2026-10-17

### Added

- `SamplerEngine::Params::streamBlocks`: blocks in a streaming voice's ring
  (`StreamWindow::kDefaultBlocks`, 4, by default). The board sets six: with
  four (279 ms of lookahead at 44.1 kHz stereo) a streamer stalled behind a
  kit load's directory walks ran voices dry -- 10 underruns in 18 swaps at
  20 hits/s; with six, 1 in 30.

## 1.68.0 — 2026-10-17

Merge of the kit-rate line (1.66.0 there) into the touch-preload line
(1.66.0 and 1.67.0 below): both sets of additions, one version.

### Added

- `crosspad/kit/KitRateConvert.hpp`: a kit's samples converted for good to
  the engine's rate. `KitRateConverter` plans (every sample's header, the
  free space, what a stopped run already wrote), writes each file into
  `<kit>/<rate>/` (`44k1/`) through `.part` and a read-back, then retargets
  the kit's JSON in one atomic save with the trims scaled; `tidyKit()`
  removes the originals that nothing names any more, `.originals` keeping
  the list across a power cut. Metadata keeps its meaning: `smpl` loops,
  `cue `, `ltxt`, `bext`, `fact` are scaled, `PEAK` and padding dropped.
  The board's I/O, memory and pacing behind `IKitConvertHost`.
- `KitInfo::keepRate` (JSON `keep_rate`): the kit is played at its own rate
  on purpose -- nothing offers to convert it. Read and written with the kit.
- `FftResamplerStream::begin(..., oversample)`: the read-out's zero-pad
  factor, 2 as before by default. The converter reads at 8x: images of a
  19 kHz tone at -69 dB instead of -30.
- `crosspad/audio/WavLayout.hpp`: the one RIFF/WAVE parser, with the
  native engine's rules (a `fmt ` of 16 bytes or more before the `data`, at
  most 64 chunk headers, the tag not looked at for 16-bit). The engine's
  `describe()` and the kit rate converter both read their headers through
  it, so a file one plays is a file the other accepts.

## 1.67.0 — 2026-10-16

A pad's voice made ready while its velocity is still being measured.

### Added

- `SamplerEngine::arm()` / `fire()` / `disarm()` and `kArmTimeoutMs`: a touch
  claims and fills a voice (sample, trim, cursor, pitch, envelope start) and
  leaves it Armed -- unheard, cutting and stealing nothing; the velocity
  fires it (level, choke and own-pad cut, room under the caps, the layer
  again where it splits by velocity or the sample was swapped). A touch that
  leaves, a hit other than the armed one, a mutation or the timeout end it.
  `Counters::armed/armFired/armCancelled/armExpired`.
- `IPadLogicHandler::onPadTouch()` (default: nothing) and
  `PadManager::handlePadTouch()`.
- `CROSSPAD_CMD_PAD_TOUCH` 0x22 (STM->ESP `[pad, touching]`), decoded by
  `Stm32MessageHandler` into `PadManager::handlePadTouch()`. An STM that does
  not send it changes nothing.

### Changed

- `SamplerEngine::noteOn()` in steps it shares with `fire()` (layer, plan,
  cut, room, fill, start); same order of effects, same output to the bit.

## 1.66.0 — 2026-10-16

The A/B switch of the audio kernels goes, now that the board has chosen
(16.10.2026: PIE fastest, and exact where it must be).

### Removed

- `dsp::KernelMode`, `kernelMode()`, `setKernelMode()` and
  `crosspad/dsp/AudioKernelsRef.hpp` (`dsp::ref::*`): a kernel runs PIE
  where it is compiled in and its buffers are on 16 bytes, the portable loop
  otherwise. Introduced in 1.64.0 for measuring and never released, so a
  minor bump: nothing outside this line called them.
- `SamplerEngine`'s frame-by-frame render (`renderVoiceRef`).
- `dsp::macPanned()`'s scratch argument, and `PitchedInstrument`'s scratch
  block for it.

### Changed

- `dsp::Biquad` lives in `AudioKernels.hpp`.
- The host tests keep the old loops as a test-local oracle; what the old
  sampler render and EQ chain played is kept as digests recorded through them.

## 1.65.0 — 2026-10-16

Merge of the note-latency line (1.63.1, 1.64.0 there) and the audio-SIMD
line (1.64.0 below): both sets of additions, one version.

### Added

- `IAudioBuffer` (`crosspad/audio/IAudioBuffer.hpp`) and
  `PlatformServices::audioBuffer`: the render block and how far ahead of the
  DAC it renders, chosen live from the choices the platform offers.
- `AudioEngineSettings::queueFrames` (NVS `cfg_audio_eng/q_frames`, JSON
  `audioEngine.queueFrames`, 0 = the platform's default, at most 4096).
- `SamplerEngine::setOverloaded()` and `Counters::sheds`: while the platform
  says its render is over budget, a note takes the oldest voice's place
  instead of adding one -- what `PitchedInstrument::setOverloaded()` does.

### Fixed

- A Loop pad longer than its head and stream ring keeps playing: the window
  numbers a looping voice's laps one after another and streams the lap's
  streamed part again; before, the voice went silent after one lap and one
  head. `SampleCursor::span()` and the block render follow the laps too.

## 1.64.0 — 2026-10-16

### Added

- `crosspad/dsp/AudioKernels.hpp`: the render's float loops -- `mac`,
  `macPanned`, `scaleStereo`, `peakStereo`, `clampPeakStereo`,
  `biquadStereo` -- branchless and one pass each, with the ESP32-S3's
  128-bit float loads and stores (`EE.LDF.128` / `EE.STF.128`) where
  `CROSSPAD_SIMD_PIE` is set (core's CMakeLists sets it for esp32s3).
  `AudioKernelsRef.hpp` keeps the loops they replace; host tests hold the
  kernels to their bits. `dsp::setKernelMode()` picks reference, portable or
  PIE at run time -- a prototype's switch, for timing both on the board.
  The dispatching functions are out of line (`src/dsp/AudioKernels.cpp`): one
  copy of each kernel in an image, not three inlined into every caller.
- `crosspad/dsp/AlignedVector.hpp`: a vector on 16 bytes, for buffers the
  PIE kernels should take.
- `SampleCursor::span()`: the frames `sample()` would read straight from one
  run, and where they are.

### Changed

- `SamplerEngine` renders a voice's block in runs with its state in locals:
  the same samples, bit for bit, in a fraction of the time.
- `EqChain` runs both channels of a section in one pass; `PitchedInstrument`
  adds a voice into the bus in one pass instead of four.
- `PitchedInstrument.hpp` no longer defines `detail::accumulatePanned()` or
  `CROSSPAD_HAS_ESP_DSP`; it is `dsp::macPanned()`.

## 1.63.0 — 2026-10-13

Merge of main's 1.55.0 line into the native-engine line (1.56.0..1.62.0, see
their `feat`/`fix` commits): both sets of features, one version.

### Changed

- `PatternSequencer`: a recorded press carries its performance (`perf`) and
  its unwrapped clock count (`elapsed`) both; `strike()` holds a note for
  what is left of its length after the poll it sounds in, for a scene's
  notes and a song block's alike.
- The audition (`previewStart()`) pans through `SamplerKit::panGains()`, so
  it plays centred at unity like a pad on the native-engine line, not 3 dB
  under.

## 1.55.0 — 2026-10-12

### Added

- `IOutputLevels` (`crosspad/audio/IOutputLevels.hpp`) and
  `PlatformServices::outputLevels`: the board's physical outputs -- speakers,
  jacks -- each with its own hardware level after the DAC and its own mute,
  and the mixer output each one carries. Null where the board has none.

## 1.54.1 — 2026-10-12

### Fixed

- A rev 1.x encoder (5-byte `CMD_ENCODER_COUNTER`, raw LPTIM count) stepped
  every second click: its steps came from A/B lines made up from the count
  on the assumption of 4 counts a click resting at 1 mod 4. Measured on
  PCB 1.9 it is 2 counts a click, and the count runs 0..60 (the STM passes
  `TIM_CHANNEL_ALL`, 0x3C, as the LPTIM period), so every wrap moved the
  detents to the other parity. The count is now unwrapped and a step taken
  where the knob lands in a detent; a wobble in one is no step.

## 1.54.0 — 2026-10-11

### Added

- `Stm32MessageHandler::setPadEdgeCallback()`: pad presses and releases from
  `CMD_BUTTONS_DIGITAL` go to the platform. A rev 1.x board's UART-only STM
  sends no note-on, so these frames are its only pad events; unset, the
  handler keeps dropping presses and releasing through `PadManager`.
- `Stm32MessageHandler::setVersionReplyCallback()`: `CMD_VERSION_REPLY`
  (`[pcb_rev, api_version]`, the rev 1.x STM's answer to a version request)
  reaches the platform instead of being dropped. So does the reply of a
  PCB 1.9 STM built before that command existed, `[F0 7D 13 api F7]` (the
  PCB number in the command byte), which read as a short status frame.

## 1.53.0 — 2026-10-09

Merge of the native sampler engine (1.49–1.52 on its branch) and WiFi remote
(RemoteFrame, enableRemote). Both lines were numbered on their own branches;
this is the first number above every one either of them used.

### Fixed (pitched crash, 2026-10-09)

- Memory generations for the pitched bank: `SampleBank` takes an optional
  `MemoryGenerationFn` (a counter its owner bumps on every wholesale wipe of
  the allocator's memory -- the device's engine arena reset), stamps itself
  with it, and once it is `stale()` carves nothing more and its destructor
  *forgets* its blocks instead of freeing them into the heap that replaced
  them ("CORRUPT HEAP: Bad head", kit_churn on hardware). `generation()`,
  `stale()` are new. `PitchedLoadEnv::generation` stamps the bank; a build
  that sees the generation change fails with "memory reset during the build"
  and frees no transient block from before the wipe. Source-compatible (the
  parameter and the field default to null). Tests: `[generation]` in
  `tests/test_SampleBank.cpp` and `tests/test_PitchedKitLoader.cpp`.

### Fixed (merge review, 2026-10-09)

- `SamplerEngine::serviceIo()` let two callers walk the voices at once (the
  streaming task and a teardown's wait); only the audition's half was
  guarded. One pass at a time now: it returns `bool`, false for the pass
  that was turned away (source-compatible for callers that ignore it).
  `ioIdle()` says no pass is in the voices or the audition.
- `SamplerEngine::abandonVoices()`: a teardown whose render is parked (the
  USB audio profile) had voices that never ended and windows that stayed
  open on freed heads. It closes every voice's ring once (Finished ones
  too), frees the voices and clears the pad bits; refused while a pass is in
  the voices. Tests: `tests/test_SamplerTeardown.cpp`.
- `crosspad/sampler/KitTake.hpp`: `TakeSpec`/`TakePadSpec`, a plain-data copy
  of what a native take or a pad rebuild reads from a kit, made on the thread
  that asks (no I/O) so the worker never reads a `KitInfo` the UI may free;
  `WorkerGate`, start-or-queue for that worker with no request lost to a
  worker on its way out; `arenaLendKeep()`/`arenaMayLend()`, the arena loan
  judged on the largest free block. Tests: `tests/test_KitTake.cpp`.
- `LatestWork<T>` (`KitTake.hpp`): the take's hand-over -- newest copy,
  `WorkerGate`, and a state derived from the gate (busy from the request to
  the worker's last pass, then the last outcome), so a retake folded into the
  pass before it can no longer leave the state "taking" for good.
- `ScopeTap`'s count is 32-bit (`written()` returns `uint32_t`): a 64-bit
  atomic is not lock-free on the board and the tap is written by the render.

### Added (audition, 2026-10-09)

- `SamplerEngine` plays one file outside the kit as an audition:
  `setPreviewSource()`, `previewStart(path, first, last, gain, balance)`,
  `previewStop()`, `previewBusy()`, and `mixPreview()` to render it on a
  channel of its own. The streaming thread (`serviceIo()`) reads the header,
  takes the voice's ring from the pool and feeds it; the note thread only
  writes a sequence-locked request. A card that falls behind is a gap, not
  the end -- for `kPreviewGapMaxMs` (2 s); a first block that never comes
  is given up after `kPreviewPrimeMaxPasses`; a stop in a gap, a header no
  engine plays, an empty pool and a kit swap (`beginMutation()`) all leave
  it idle with its ring back. `previewAbandon()` for a teardown whose render
  is parked. What the prebuilt streamer's spare voice did for the file
  browser and the start-up sound. Tests: `tests/test_SamplerPreview.cpp`
  (22, nine of them failure paths).

### Changed (image size, 2026-10-09)

- The set-up, take and report functions of the files a board builds `-O2`
  for their render (`SamplerEngine`, `BlockPool`, `PitchedInstrument`) are
  marked `CROSSPAD_COLD` (GCC `cold`: built for size, behaviour unchanged).
  The render, note and refill paths are untouched.

### Added (WiFi remote, feat/remote-frame — numbered 1.48.0 on its branch)

- `crosspad/net/RemoteFrame.hpp`: `parseRemoteRequest()`, `formatRemoteReply()` — the `<id> <line>` frame of the WiFi remote (MQTT `cp/{serial}/req|rsp`), limits `kRemoteCommandMax` 512 / `kRemoteReplyMax` 4096. Tests: `tests/test_remote_frame.cpp`.
- `WirelessSettings::enableRemote` — WiFi remote management (default on; NVS cfg_wireless/remote, JSON enableRemote)

### Added (native sampler engine, feat/native-sampler-engine — 1.49–1.52 on its branch, 1.40–1.48 before that)

- `crosspad/sampler/*`: a streaming sampler engine of our own -- `BlockPool`,
  `SampleStore`, `StreamWindow`, `SamplerKit`, `SamplerEngine` (voices, kit,
  card streaming budget, per-note transpose and hit key).
- Instrument host (`IInstrumentHost`, `InstrumentPlayState`,
  `InstrumentPadLogic`), the looper (`LooperEngine`, `LooperAccess`),
  `OutputEq`, more scales (Lydian, Phrygian), ninth chords and progressions.
- Song: clip sets, a live transport, four tracks with a seq track, edit by
  bars, seq blocks laid on the song map, one transport tempo.
- `IEventBus::postSamplePlayback` takes a transpose and a key.

### Merge notes

- `PatternSequencer` note lengths were written twice: main's (bc0789a,
  `noteRelease`) and the engine branch's (80f1a25, `noteOut`, plus the song
  map and Record-replaces). The engine branch's sequencer is kept;
  `noteRelease()` stays as an alias of `noteOut()`, and main's refinements are
  carried over: lengths measured on `MetronomeNode::elapsedSamples` (a hold
  over the loop end is not short, longer than the loop is the loop),
  `sceneEventLength` of a pad still down grows while it is held, a held
  note's release is counted from where it sounds, and the poll's press
  buffer is in the object, sized for a release per note.
- `EspEventBus::postSamplePlayback`: the engine's own voice reports
  (`EventSource::SamplePlayback`) are still not posted (main, 054d14e).
- `SongEngine`: the ring is the engine branch's 16384 frames with a 4096
  prime; a bounce's first piece still fills the WAV header's 4 kB (main);
  merge leaves out muted, silent and seq tracks; `requestImport` checks its
  target track before it claims the edit.

## 1.48.1 — 2026-10-08

### Fixed

- `KitJsonParser`: kits made on crosspad.app carry a per-pad `"mode"`
  (tap / hold / toggle / cycle) instead of `playbackMode` / `cycle`; the
  parser dropped it and every pad played one-shot. An explicit
  `playbackMode` or `cycle` still wins.

## 1.48.0 — 2026-10-08

Linking a board to a crosspad.app account (from 341857b on feat/cloud-builder,
without the custom tier): `crosspad/fw/FwAccount.hpp` -- `fwAccountUrlAllowed`
(the device token goes only under the compile-time account base, byte for
byte), pairing code and device token shapes, and the `pair/start` /
`pair/token` reply parsers. Tests: `tests/test_fw_account.cpp`.

## 1.47.0 — 2026-10-08

### Added

- `TouchSensitivity` (Low / Medium / High) and
  `CrosspadSettings::touchSensitivity`, NVS `cfg_display/touch_sens`
  (default Low = the threshold the panel always ran at, out-of-range
  values clamp), carried by the settings
  backup as `touchSensitivity`. The platform maps a level to its touch
  controller's threshold.

## 1.46.1 — 2026-10-07

Review fixes (fix/steve-review: 802e51a, 3ed896d).

### Fixed

- `SongEngine`: opening another project while a commit waits out the save
  delay first saves that commit to the song it was made in. The edit used to
  be lost, and the delayed save wrote the newly opened song.
- `SongEngine` merge: when the target track keeps blocks outside the range,
  its volume stays and the clip is mixed relative to it; it used to go to
  unity and change the loudness of those blocks. Tracks at volume 0 are left
  out like muted ones. (The mix still saturates at 16 bits, so a quiet target
  merged with loud tracks can clip.)
- `SongEngine` edits run one at a time: a rename, delete, tempo, merge,
  import or export requested while `editState()` is Busy is refused, and the
  parameters of the edit in flight are no longer overwritten by the next
  request.

### Removed

- `PadAnimator::setProgressColor()`: no caller left (the boot splash draws
  with `showFrame()` since 1.36.0; nothing in platform-idf, crosspad-pc or
  the apps calls it).

## 1.46.0 — 2026-10-07

The sequencer note length (#21, bc0789a/bc4e3ef), which was merged into
release/core-1.41.0 after that branch had gone to main and so never reached it.

### Added

- A recorded note is as long as its pad was held. `PatternSequencer::noteRelease()`
  (wait-free, like `noteIn()`) closes the note its press opened; a note whose pad
  is still down grows under the finger. Playback holds the pad down for the
  length and releases it inside the window it runs out in; a tap still gets one
  poll of gate.
- `ISequencerEngine::sceneEventLength()` and
  `SequencerController::getSceneEventLength()`: an event's length as a fraction
  of the scene's loop (0 for a tap), for drawing.
- `PatternSequencer::eventLength()`, `eventLengthTicks()`, and a `lenTicks`
  argument to `addEventTicks()` (default 0, a tap).
- `MetronomeNode::elapsedSamples()`: samples rendered while running, never
  wrapped or reset -- what a hold is measured on.

### Changed

- Scene files: a held note is saved as `[ticks, pad, vel, len]`; a tap stays a
  triple, and files without lengths load as taps.
- A scene change or a new bar count releases every pad held by a length;
  `stop()` closes the notes of pads still down.
- `poll()` collects its presses in a member buffer (`2 * kMaxEvents + 32`)
  instead of on the poll task's stack.

## 1.45.0 — 2026-10-07

The firmware sale line -- 1.42.0, 1.43.0 and the press debounce
(fix/latency, 8903372) -- merged onto main's 1.44.1 (Firmware Store,
IInputPairs).

### Added

- `Stm32Settings::pressDebounceMs`: how long a pad must stay pressed before
  the STM sends its note-on, which is also the velocity window. 6 ms on every
  board profile (`BOARD_PRESS_DEBOUNCE_MS_DEF`) where the STM builds 15 ms;
  NVS key `press_db`, JSON `pressDebounceMs` clamped to 2..15.
  `Stm32ConfigSync::pushAll` writes it (`REG_CTL_BTN_PRESS_DB_MS`, 0x033C) on
  boot and after every STM reset; release debounce stays the STM's.
- `CrosspadStatus::batteryLow` and the SoC flags `REG_SOC_FLAG_NO_BATTERY` /
  `REG_SOC_FLAG_LOW_BATTERY`; a negative `batteryPct` means no battery fitted.
- `PitchedInstrument::setVoiceCap()` / `voiceCap()` / `maxVoices()`: the
  platform's ceiling on polyphony, applied by every `setParams()`.
  `setOverloaded()` / `overloaded()` / `shedCount()`: while the render is over
  its budget a new note takes a sounding voice's place instead of adding one.
- `ISongEnginePlatform::writeIo()` (default: `fwrite`), `WavWriter::kHeaderBytes`,
  `stream()` and `advance()`; `BounceStatus` reports `chunks`, `chunkAvgUs`,
  `chunkMaxUs` and `drainKBps`.
- `SpeakerAmpGate::signalWithin()`: output carried signal recently, whether or
  not the speaker amp may run.

### Changed

- A bounce writes whole 4 kB pieces that end on 4 kB boundaries of the file,
  through `writeIo()`; "card too slow" also reports the drain rate.
- A stolen voice fades over 5 ms (`kStealFadeMs`, was 2 ms), and a voice
  already in its release is stolen before a held one.
- `PadLedController`, coalesced: `notifyNotePlay()` from the audio render only
  records the state; `update()` paints it under the strip lock.
- `EspEventBus` no longer posts the engine's voice reports
  (`EventSource::SamplePlayback`); requests to play are still posted.
- `Stm32Manager`: audible playback (speaker or headphones) counts as user
  activity for the STM's dim/sleep timers, at most every 5 s; the LCD
  brightness is resent after an STM reset.

### Fixed

- `Stm32ConfigSync::pushAll` no longer writes `REG_CTL_EN_3V3_HOLD`, which
  released the ESP rail to HiZ on every push.

### Version numbers

1.42.0 (release/core-1.42.0, 62ba0ba) and 1.43.0 (release/core-1.43.0,
c693ad9) were released only on the sale line while main went from 1.41.0 to
1.44.1, so 1.44.x does not contain them. A second, different tree is also
labelled 1.43.0: bc4e3ef on feat/seq-note-length (sequencer note length).
Gate on >= 1.45.0 for anything listed above.

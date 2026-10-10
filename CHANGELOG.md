# Changelog

Notable changes to crosspad-core. The version is the triple in
`include/crosspad/CrosspadCoreVersion.hpp` (bump rules there; `library.json`
carries the same number) and consumers gate on it with
`crosspad/RequireVersion.hpp`. This file starts at 1.45.0; earlier releases
are described in their `chore: X.Y.Z` commits.

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

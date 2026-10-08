# Changelog

Notable changes to crosspad-core. The version is the triple in
`include/crosspad/CrosspadCoreVersion.hpp` (bump rules there; `library.json`
carries the same number) and consumers gate on it with
`crosspad/RequireVersion.hpp`. This file starts at 1.45.0; earlier releases
are described in their `chore: X.Y.Z` commits.

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

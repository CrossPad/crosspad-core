// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

/**
 * @file Transport.hpp
 * @brief One transport for the whole instrument.
 *
 * Before this there were eight hand-rolled button rows and six unrelated
 * state models: the sequencer's `SequencerMode` + `SequencerState`, the song
 * engine's `playing()` + `SyncTake`, the looper's `LoopState`, the recorder's
 * `RecStatus::recording`, the host's mirrored `kTransport*` bits, and
 * `MidiClock::running()`. Every app re-derived "is it playing?" its own way
 * and no two of them agreed on what PLAY meant.
 *
 * So: one state, one set of four commands, and a **target** an app registers.
 * Whoever holds the claim is what PLAY starts. Two things never play at once,
 * because a new claim stops the one before it.
 *
 * The transport does not know what a pattern, a scene, a loop or a take is --
 * that stays with the app. It knows only what the button row needs: which of
 * the four commands make sense here, and what is happening now.
 *
 * Published on its own rather than through PlatformServices, like the looper:
 * a platform can leave it unset and every caller has to check anyway.
 */

namespace crosspad {

/// What the instrument is doing. One enum for the bar to paint from, however
/// many state machines sit behind the claim.
enum class TransportState : uint8_t {
    Stopped = 0,
    Playing,
    Recording,
    Overdubbing,
    /// Armed and waiting: a sync point, a count-in, or the first note.
    Waiting,
};

/// Where the transport is. 1-based bar and beat; `cent` is hundredths of a
/// beat, not MIDI ticks -- the bar prints two digits and a resolution nobody
/// can read is a resolution worth not inventing.
struct TransportPos {
    uint16_t bar  = 1;
    uint8_t  beat = 1;
    uint8_t  cent = 0;
};

/// Which of the four commands a target can honour. A bar greys the rest
/// rather than offering a button that does nothing.
constexpr uint8_t kTransportCanPlay    = 1u << 0;
constexpr uint8_t kTransportCanStop    = 1u << 1;
constexpr uint8_t kTransportCanRecord  = 1u << 2;
constexpr uint8_t kTransportCanOverdub = 1u << 3;
/// Where the next PLAY starts can be moved by bars (ITransportTarget::locate):
/// the bar's position readout is a control then, not only a readout.
constexpr uint8_t kTransportCanLocate  = 1u << 4;

/**
 * @brief What a transport command reaches.
 *
 * Implemented by whatever an app wants PLAY to start -- the pattern
 * sequencer, the audio song, the looper, a take. The bar never learns which.
 */
class ITransportTarget {
public:
    virtual ~ITransportTarget() = default;

    virtual void play() = 0;
    virtual void stop() = 0;
    /// Default to nothing rather than to play: a target that cannot record
    /// should do nothing when asked, and say so through caps().
    virtual void record() {}
    virtual void overdub() {}

    /// What is happening, asked whenever the bar repaints. Must be cheap and
    /// safe to call from the UI thread.
    virtual TransportState state() const = 0;

    virtual uint8_t caps() const { return kTransportCanPlay | kTransportCanStop; }

    /// For the bar's label, e.g. "PATTERN", "SONG", "LOOP". Never null.
    virtual const char* name() const { return ""; }

    /// The tempo the target runs at, or 0 when it has none of its own (a
    /// take, a one-shot) -- the bar then leaves its BPM field blank rather
    /// than printing a number nobody set.
    virtual float bpm() const { return 0.0f; }

    /// Where the transport is, for the bar's readout. Bars and beats are
    /// 1-based, the way a musician counts them; @ref TransportPos::cent is
    /// hundredths of a beat, which is what fits the two digits the design
    /// gives the field.
    virtual TransportPos position() const { return {}; }

    /// Move where the next PLAY starts by @p bars, back for a negative
    /// count. Called only while stopped and only with kTransportCanLocate in
    /// caps(); a target that has no position of its own leaves it out.
    virtual void locate(int bars) { (void)bars; }
};

/**
 * @brief Take the transport.
 *
 * The previous holder is **stopped** first, if it is running: one transport
 * means one thing sounding, and a claim is how an app says "PLAY now means
 * me". Claiming the target that already holds it does nothing.
 *
 * @p stopPrevious false hands over without stopping, for two targets that
 * drive the same playback -- Arrange's song and the seq view of one of its
 * tracks, which plays that song too.
 *
 * A claim outlives the app's screen on purpose. Leaving the Sequencer does
 * not stop the pattern today and should not start doing so because the
 * buttons moved to the bar.
 */
void transportClaim(ITransportTarget* target, bool stopPrevious = true);

/// Give it back, if @p target is the one holding it. Does not stop anything:
/// a target that wants silence stops itself first.
void transportRelease(ITransportTarget* target);

ITransportTarget* transportTarget();

/* The four commands. Each is a no-op with no target, or where caps() says
 * the target cannot do it -- so a bar may call them without checking. */
void transportPlay();
void transportStop();
void transportRecord();
void transportOverdub();

/// Stopped with no target, so a bar can paint before anything has claimed.
TransportState transportState();
uint8_t        transportCaps();
const char*    transportName();
/// 0 with no target, or where the target has no tempo of its own.
float          transportBpm();
TransportPos   transportPosition();
/// Where the next PLAY starts, @p bars on (or back). A no-op unless the target
/// is stopped and says kTransportCanLocate.
void           transportLocate(int bars);

// ── The tempo ───────────────────────────────────────────────────────────────
//
// One tempo for the instrument. The sequencer's clock is where it lives -- the
// pattern, the click and the MIDI clock the board sends all run on it -- and
// an open song is put on it whenever it is set here, so the Sequencer, Song
// and Arrange never show two numbers after an edit. A song still carries its
// own tempo in song.json (its clips were made at it), and playing it puts
// that on the clock, as before.

/// Tempos the transport accepts, in BPM: the sequencer's own limits.
constexpr float kTransportTempoMin = 20.0f;
constexpr float kTransportTempoMax = 300.0f;

/// The tempo everything plays at now. 0 before a sequencer is published.
float transportTempo();
/// Set it, clamped to the limits: the sequencer and its click at once, so a
/// turn is heard while it is made -- except under a song that is playing,
/// whose audio cannot follow; then only transportCommitTempo() moves it.
void  transportSetTempo(float bpm);
/// The edit is done: the tempo of transportSetTempo()'s last call reaches the
/// open song too (re-measured and saved by its worker, which stops a song
/// that is playing), and the clock if a song held it back.
void  transportCommitTempo();
/// True while a host's MIDI clock drives the tempo: an edit would be undone
/// at its next tick, so a control offering one should say so instead.
bool  transportTempoFromHost();
/// The platform's MIDI clock, each time the source of its tempo changes.
void  transportSetTempoFromHost(bool host);

/// The metronome: the click of the clock the tempo lives on, heard whenever
/// something plays on it -- a pattern, a song with its seq. One switch, the
/// player's; no transport command flips it. A count-in counts either way.
/// False before a sequencer is published.
bool  transportClick();
void  transportSetClick(bool on);

} // namespace crosspad

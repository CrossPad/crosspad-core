// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

/**
 * @file Transport.hpp
 * @brief One transport for the whole instrument.
 *
 * One state, one set of four commands (play, stop, record, overdub), and a
 * **target** an app registers.
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

    /// Start playing.
    virtual void play() = 0;
    /// Stop.
    virtual void stop() = 0;
    /// Start recording. Default to nothing rather than to play: a target that
    /// cannot record should do nothing when asked, and say so through caps().
    virtual void record() {}
    /// Start overdubbing. Default: nothing, as for record().
    virtual void overdub() {}

    /// What is happening, asked whenever the bar repaints. Must be cheap and
    /// safe to call from the UI thread.
    virtual TransportState state() const = 0;

    /// Which commands the target honours: kTransportCan* bits. Default:
    /// play and stop.
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
 * A claim outlives the app's screen while its target plays: leaving an app
 * does not stop what it plays. A holder left stopped goes back to the home
 * target (transportGoHome()).
 */
void transportClaim(ITransportTarget* target, bool stopPrevious = true);

/// The target the transport belongs to when no app holds it in earnest --
/// the pattern sequencer on the board -- claimed now. nullptr forgets it.
void transportSetHome(ITransportTarget* target);

/// Back to the home target if another one holds the transport and stands
/// stopped; a holder that plays, records or counts in keeps it, so its STOP
/// stays on the bar. The UI calls this as an app opens and closes: a screen
/// with no transport of its own (the Sampler) records the pattern, not a
/// song another app left stopped behind it.
void transportGoHome();

/// Give it back, if @p target is the one holding it. Does not stop anything:
/// a target that wants silence stops itself first.
void transportRelease(ITransportTarget* target);

/// The target holding the transport, or null.
ITransportTarget* transportTarget();

/// PLAY on the holder. Like the other three commands, a no-op with no target
/// or where caps() says the target cannot do it, so a bar may call them
/// without checking.
void transportPlay();
/// STOP on the holder (see transportPlay()).
void transportStop();
/// RECORD on the holder (see transportPlay()).
void transportRecord();
/// OVERDUB on the holder (see transportPlay()).
void transportOverdub();

/// The holder's state; Stopped with no target, so a bar can paint before
/// anything has claimed.
TransportState transportState();
/// The holder's caps(); 0 with no target.
uint8_t        transportCaps();
/// The holder's name(); "" with no target.
const char*    transportName();
/// 0 with no target, or where the target has no tempo of its own.
float          transportBpm();
/// The holder's position(); bar 1, beat 1 with no target.
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
/// Turn the metronome on or off.
void  transportSetClick(bool on);

} // namespace crosspad

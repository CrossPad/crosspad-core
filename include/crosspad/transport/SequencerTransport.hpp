// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sequencer/SequencerController.hpp"
#include "crosspad/sequencer/SequencerTypes.hpp"
#include "crosspad/song/ISongEngine.hpp"
#include "crosspad/transport/Transport.hpp"

#include <mutex>

/**
 * @file SequencerTransport.hpp
 * @brief The pattern sequencer, as something the transport can drive.
 *
 * The first ITransportTarget, and the one the bar starts out holding: a board
 * at the launcher with nothing open still has PLAY mean "play the pattern".
 *
 * Nothing here decides anything -- the controller already owns the modes and
 * the engine owns the clock. This translates between the five SequencerState
 * values and the five the bar paints from, and it carries the four rules of
 * the Sequencer's PLAY/STOP/REC/DUB: a song-synced start, a counted-in take,
 * the second REC that closes a take, and stopping the song with the pattern.
 * They live here, where every transport -- the bar, a MIDI host, a verb --
 * gets them, rather than in one screen.
 */

namespace crosspad {

class SequencerTransport final : public ITransportTarget {
public:
    explicit SequencerTransport(SequencerController& seq) : seq_(seq) {}

    /// The seq view of an Arrange track: PLAY and REC play the arrangement
    /// with the track's scene laid on it, as SYNC does, whatever SYNC says --
    /// a part is recorded against the song it belongs to, not in silence. The
    /// Sequencer opened on its own clears it.
    void setFollowSong(bool on) { followSong_ = on; }
    bool followsSong() const { return followSong_; }

    /* The table in UX 2000 section 3.1, one command per row. A command the
     * state has no row for does nothing -- caps() greys it on the bar. */

    /// From a stop: play on from where the last stop left the pattern.
    /// Recording or overdubbing: out of the take, playing on without a break
    /// -- PLAY is the other way out of REC and DUB, beside pressing them again
    /// (a departure from the table, asked for on the board).
    void play() override
    {
        const TransportState s = state();
        if (s == TransportState::Recording || s == TransportState::Overdubbing) { seq_.playback(); return; }
        if (s != TransportState::Stopped) return;
        if (!startWithSong(SyncTake::None)) seq_.playback();
    }

    /// Stops in place, cancels an armed take, and the song too when it is the
    /// one running the sequencer. Stopped already, it goes back to bar one --
    /// and so does a take it ends, recorded or armed, so PLAY plays the take
    /// from its start rather than from where the STOP fell in the loop.
    void stop() override
    {
        if (state() == TransportState::Stopped) { seq_.rewind(); return; }
        const SequencerMode m = seq_.getMode();
        const bool take = m == SequencerMode::Record || m == SequencerMode::Overdub ||
                          m == SequencerMode::OverdubWait;
        ISongEngine* se = getPlatformServices().songEngine;
        if (se && se->playing() && songSync()) se->stop();
        seq_.stop();
        if (take) seq_.rewind();
    }

    /// Arms a take from a stop (counted in with PRE on), punches in while
    /// playing or overdubbing, punches out while recording, and cancels a
    /// take still waiting for its first beat.
    void record() override
    {
        switch (state()) {
            case TransportState::Stopped:
                if (!startWithSong(SyncTake::Record)) seq_.record();
                return;
            case TransportState::Waiting:     stop();          return;
            case TransportState::Recording:   seq_.playback(); return;
            case TransportState::Playing:
            case TransportState::Overdubbing: seq_.record();   return;
        }
    }

    /// Layers onto the running loop, and a second DUB stops layering.
    void overdub() override
    {
        switch (state()) {
            case TransportState::Playing:
            case TransportState::Recording:   seq_.overdub();  return;
            case TransportState::Overdubbing: seq_.playback(); return;
            case TransportState::Stopped:
            case TransportState::Waiting:                      return;
        }
    }

    TransportState state() const override {
        switch (seq_.getState()) {
            case SequencerState::Idle:
                /* Following, the song primes or counts in before it starts
                 * the sequencer: that is armed, not stopped. */
                if (followSong_) {
                    ISongEngine* se = getPlatformServices().songEngine;
                    if (se && se->playing()) return TransportState::Waiting;
                }
                return TransportState::Stopped;
            /* Armed and counting: a sync point, the count-in, or the first
             * note the player has not hit yet. The bar brackets PLAY and
             * offers STOP, which is exactly what cancels all three. */
            case SequencerState::WaitForSync:
            case SequencerState::WaitNote:
            case SequencerState::PreBeat:
                return TransportState::Waiting;
            case SequencerState::Running:
                break;
        }
        switch (seq_.getMode()) {
            case SequencerMode::Record:      return TransportState::Recording;
            case SequencerMode::Overdub:
            /* Still counting to the overdub's entry, but the pattern under it
             * is already sounding -- so it is Overdubbing, not Waiting. */
            case SequencerMode::OverdubWait: return TransportState::Overdubbing;
            case SequencerMode::Playback:
            case SequencerMode::None:        break;
        }
        return TransportState::Playing;
    }

    /// Armed, only STOP or REC (both cancel); stopped, no DUB -- there is
    /// no loop running to layer onto.
    uint8_t caps() const override {
        switch (state()) {
            case TransportState::Waiting:
                return kTransportCanStop | kTransportCanRecord;
            case TransportState::Stopped:
                return kTransportCanPlay | kTransportCanStop | kTransportCanRecord;
            default:
                return kTransportCanPlay | kTransportCanStop |
                       kTransportCanRecord | kTransportCanOverdub;
        }
    }

    const char* name() const override { return "PATTERN"; }

    float bpm() const override { return seq_.getBpm(); }

    /* The engine reports one number -- how far through the whole pattern it
     * is, 0..1 -- so bar, beat and hundredths all come out of it rather than
     * from three counters that could disagree. Four beats to the bar: the
     * sequencer has no other meter. */
    TransportPos position() const override {
        TransportPos p;
        /* Following, where the song is: the part is placed on its bars. */
        if (followSong_) {
            ISongEngine* se = getPlatformServices().songEngine;
            if (se && se->playing()) {
                double bars = se->positionBars();
                if (bars < 0.0) bars = 0.0;
                const uint32_t whole = (uint32_t)bars;
                const double beats = (bars - (double)whole) * 4.0;
                const uint32_t beat = (uint32_t)beats;
                p.bar  = (uint16_t)(whole + 1u);
                p.beat = (uint8_t)(beat + 1u);
                p.cent = (uint8_t)((beats - (double)beat) * 100.0);
                if (p.cent > 99) p.cent = 99;
                return p;
            }
        }
        /* The *scene's* bars, not getSeqLength(): that is the controller's
         * global maximum (the platform sets it to MAX_SEQ_LENGTH at init),
         * and dividing the progress by it put a one-bar scene at bar 8 --
         * visible on the screen, invisible in every number the board
         * reports. */
        const uint8_t bars = seq_.getSceneBars(seq_.getActiveScene());
        if (bars == 0) return p;
        const float totalBeats = (float)bars * 4.0f;
        float beats = seq_.getProgress() * totalBeats;
        if (beats < 0.0f) beats = 0.0f;
        /* progress() can read exactly 1.0 at the loop point; wrapping here
         * keeps the display off bar N+1, which never exists. */
        if (beats >= totalBeats) beats = 0.0f;
        const uint32_t whole = (uint32_t)beats;
        p.bar  = (uint16_t)(whole / 4u + 1u);
        p.beat = (uint8_t)(whole % 4u + 1u);
        p.cent = (uint8_t)((beats - (float)whole) * 100.0f);
        if (p.cent > 99) p.cent = 99;
        return p;
    }

private:
    /// SYNC on (or following an Arrange track) and a song to play: the
    /// transport starts the song, and the song starts the sequencer with it
    /// (Arrange's SYNC is the same switch).
    bool songSync() const
    {
        ISongEngine* se = getPlatformServices().songEngine;
        if (!se) return false;
        bool sync = followSong_;
        {
            std::lock_guard<std::recursive_mutex> l(se->songMutex());
            sync = sync || se->song().syncSeq;
        }
        return sync && se->playEndBars() > 0;
    }

    /// From a stop, in sync: the song from its top, the sequencer doing @p take.
    ///
    /// The count-in belongs to the take, and in sync it is the metronome that
    /// plays it -- the song and the scene both start on the sample after the
    /// last click. Plain play is immediate, with the song or without it.
    bool startWithSong(SyncTake take) const
    {
        ISongEngine* se = getPlatformServices().songEngine;
        if (!se || seq_.getState() != SequencerState::Idle || !songSync()) return false;
        const uint8_t beats = take != SyncTake::None ? seq_.getCountInBeats() : 0;
        return beats ? se->playCountedIn(0, take, beats) : se->playWithTake(0, take);
    }

    SequencerController& seq_;
    bool followSong_ = false;
};

/**
 * @brief The board's one pattern transport.
 *
 * Shared rather than built per caller: transportClaim() stops the holder it
 * replaces, so a second adapter over the same controller would stop the
 * pattern every time the Sequencer screen opened. Null until the platform has
 * published a SequencerController.
 */
SequencerTransport* sequencerTransport();

} // namespace crosspad

// SPDX-License-Identifier: MIT
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
 * at the launcher with nothing open still has PLAY mean "play the pattern",
 * which is what it meant on the 2000.
 *
 * Nothing here decides anything -- the controller already owns the modes and
 * the engine owns the clock. This translates between the five SequencerState
 * values and the five the bar paints from, and it carries the four rules the
 * Sequencer screen's own PLAY/STOP/REC/DUB buttons used to carry before the
 * transport moved to the top bar. Those rules were lost for a build: the bar
 * called plain playback()/record() and a song-synced start, a counted-in
 * take, the second REC that closes a take, and stopping the song with the
 * pattern all stopped happening. They belong here, where every transport --
 * the bar, a MIDI host, a verb -- gets them, rather than in one screen.
 */

namespace crosspad {

class SequencerTransport final : public ITransportTarget {
public:
    explicit SequencerTransport(SequencerController& seq) : seq_(seq) {}

    void play() override
    {
        if (!startWithSong(SyncTake::None)) seq_.playback();
    }

    /// The song too, when it is the one running the sequencer.
    void stop() override
    {
        ISongEngine* se = getPlatformServices().songEngine;
        if (se && se->playing() && songSync()) se->stop();
        seq_.stop();
    }

    /// A second REC closes the take without stopping the loop, which is what
    /// you want once the part is played in.
    void record() override
    {
        if (seq_.isRecording())                      { seq_.playback(); return; }
        if (!startWithSong(SyncTake::Record))          seq_.record();
    }

    void overdub() override
    {
        if (!startWithSong(SyncTake::Overdub)) seq_.overdub();
    }

    TransportState state() const override {
        switch (seq_.getState()) {
            case SequencerState::Idle:
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

    uint8_t caps() const override {
        return kTransportCanPlay | kTransportCanStop |
               kTransportCanRecord | kTransportCanOverdub;
    }

    const char* name() const override { return "PATTERN"; }

    float bpm() const override { return seq_.getBpm(); }

    /* The engine reports one number -- how far through the whole pattern it
     * is, 0..1 -- so bar, beat and hundredths all come out of it rather than
     * from three counters that could disagree. Four beats to the bar: the
     * sequencer has no other meter. */
    TransportPos position() const override {
        TransportPos p;
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
    /// SYNC on and a song to play: the transport starts the song, and the
    /// song starts the sequencer with it (Arrange's SYNC is the same switch).
    static bool songSync()
    {
        ISongEngine* se = getPlatformServices().songEngine;
        if (!se) return false;
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        return se->song().syncSeq && se->song().playEnd() > 0;
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

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sequencer/SequencerController.hpp"
#include "crosspad/song/ISongEngine.hpp"
#include "crosspad/transport/Transport.hpp"

#include <mutex>

/**
 * @file SongTransport.hpp
 * @brief The arranged song, as something the transport can drive.
 *
 * The second ITransportTarget. Without it the Arranger was silent: PLAY in
 * the top bar still belonged to the pattern sequencer, which is what the
 * board claims at boot, so opening a song and pressing PLAY started an empty
 * scene instead of the arrangement. The song only ever started by accident --
 * through the sequencer's SYNC, and only when SYNC was on.
 *
 * Arrange claims this on open. The claim outlives the screen, as every claim
 * here does: the last thing you opened is what PLAY means.
 *
 * REC and DUB record the pads onto the song's seq track from where PLAY would
 * start (ISongEngine::recordSeq): a REC is a block of its own notes there, as
 * many bars as it ran; a DUB adds to the blocks it plays over. Pressed while
 * the song plays they punch in, pressed again they punch out.
 */

namespace crosspad {

class SongTransport final : public ITransportTarget {
public:
    /// Locate: where the next PLAY starts. Arrange sets it from its
    /// selection, so choosing a block is choosing where the song plays from.
    void setStartBar(uint32_t bar) { heldBar_ = bar; }
    /// Where the next PLAY starts, 0-based: the playhead while stopped.
    uint32_t startBar() const { return heldBar_; }

    /// From a stop, from the held bar -- where the last stop or a locate left
    /// it -- or from the loop region's start while looping one. In a take:
    /// out of it, playing on, as the pattern's PLAY is.
    void play() override
    {
        ISongEngine* se = engine();
        if (!se) return;
        if (se->playing()) {
            if (se->seqTake() != SyncTake::None) se->seqPunch(SyncTake::None);
            return;
        }
        se->play(startFrom(*se));
    }

    /// Stops and holds the bar it stopped in; stopped already, back to bar one.
    void stop() override
    {
        ISongEngine* se = engine();
        if (!se) return;
        if (!se->playing()) { heldBar_ = 0; return; }
        double bars = se->positionBars();
        heldBar_ = bars > 0.0 ? (uint32_t)bars : 0;
        se->stop();
    }

    /// From a stop, counted in as the pattern's PRE says; armed, called off;
    /// playing, punched in; in a REC, punched out; in a DUB, a REC from here.
    void record() override
    {
        ISongEngine* se = engine();
        if (!se) return;
        switch (state()) {
            case TransportState::Stopped:     se->recordSeq(startFrom(*se), SyncTake::Record, countIn()); return;
            case TransportState::Waiting:     stop();                                return;
            case TransportState::Recording:   se->seqPunch(SyncTake::None);          return;
            case TransportState::Playing:
            case TransportState::Overdubbing: se->seqPunch(SyncTake::Record);        return;
        }
    }

    /// The same for a DUB: what is played is added to what is there.
    void overdub() override
    {
        ISongEngine* se = engine();
        if (!se) return;
        switch (state()) {
            case TransportState::Stopped:     se->recordSeq(startFrom(*se), SyncTake::Overdub, countIn()); return;
            case TransportState::Waiting:                                            return;
            case TransportState::Overdubbing: se->seqPunch(SyncTake::None);          return;
            case TransportState::Playing:
            case TransportState::Recording:   se->seqPunch(SyncTake::Overdub);       return;
        }
    }

    /// A counted-in start is armed (amber on the bar) until its beat one.
    TransportState state() const override
    {
        ISongEngine* se = engine();
        if (!se || !se->playing()) return TransportState::Stopped;
        if (se->countInBeatsLeft()) return TransportState::Waiting;
        switch (se->seqTake()) {
            case SyncTake::Record:  return TransportState::Recording;
            case SyncTake::Overdub: return TransportState::Overdubbing;
            case SyncTake::None:    break;
        }
        return TransportState::Playing;
    }

    /// REC and DUB where there is a track for the take -- the song's seq
    /// track, or a free one to make it on -- and, playing, a sequencer locked
    /// to the song to keep the hits.
    uint8_t caps() const override
    {
        ISongEngine* se = engine();
        const uint8_t take = kTransportCanRecord | kTransportCanOverdub;
        switch (state()) {
            case TransportState::Waiting: return kTransportCanStop | kTransportCanRecord;
            case TransportState::Stopped:
                return kTransportCanPlay | kTransportCanStop | kTransportCanLocate | (se && takeRoom(*se) ? take : 0);
            case TransportState::Playing:
                return kTransportCanPlay | kTransportCanStop | (se && se->seqPunchable() ? take : 0);
            default:
                return kTransportCanPlay | kTransportCanStop | take;
        }
    }

    /// The bar's position turned while stopped: a bar a detent, never before
    /// the song's bar one.
    void locate(int bars) override
    {
        const int64_t to = (int64_t)heldBar_ + bars;
        heldBar_ = (uint32_t)(to < 0 ? 0 : (to > kMaxBar ? kMaxBar : to));
    }

    /// "ARR": the block song of the Song screen is "SONG".
    const char* name() const override { return "ARR"; }

    /// The song's tempo, which playing it puts on the clock; a song with no
    /// tempo yet runs at the instrument's (transportTempo()).
    float bpm() const override
    {
        ISongEngine* se = engine();
        if (!se) return 0.0f;
        float b = 0.0f;
        {
            std::lock_guard<std::recursive_mutex> l(se->songMutex());
            b = se->song().bpm;
        }
        return b > 0.0f ? b : transportTempo();
    }

    /// The engine counts in bars, fractionally; the bar's readout wants a
    /// bar, a beat and hundredths of one. Four beats to the bar, which is
    /// what AudioSong's own metre assumes.
    TransportPos position() const override
    {
        TransportPos p;
        ISongEngine* se = engine();
        if (!se) return p;
        /* Stopped, where the next PLAY starts. */
        double bars = se->playing() ? se->positionBars() : (double)heldBar_;
        if (bars < 0.0) bars = 0.0;
        const uint32_t whole = (uint32_t)bars;
        const double frac = bars - (double)whole;
        const double beats = frac * 4.0;
        const uint32_t beat = (uint32_t)beats;
        p.bar  = (uint16_t)(whole + 1u);
        p.beat = (uint8_t)(beat + 1u);
        p.cent = (uint8_t)((beats - (double)beat) * 100.0);
        if (p.cent > 99) p.cent = 99;
        return p;
    }

private:
    static ISongEngine* engine() { return getPlatformServices().songEngine; }

    /// Where PLAY and REC start: the held bar, or the region's start while
    /// looping one.
    uint32_t startFrom(ISongEngine& se) const
    {
        std::lock_guard<std::recursive_mutex> l(se.songMutex());
        const AudioSong& song = se.song();
        return song.loop && song.hasRegion() ? song.loopStart : heldBar_;
    }

    /// The pattern's PRE: a take in the song counts in as one in a scene does.
    static uint8_t countIn()
    {
        SequencerController* sq = getPlatformServices().sequencer;
        return sq ? sq->getCountInBeats() : 0;
    }

    /// A seq track to take onto, or room to make one.
    static bool takeRoom(ISongEngine& se)
    {
        std::lock_guard<std::recursive_mutex> l(se.songMutex());
        const AudioSong& song = se.song();
        return song.seqTracks() > 0 || song.freeTrack() >= 0;
    }
    /// Arrange's last bar (kMaxLength), 0-based.
    static constexpr int64_t kMaxBar = 998;

    uint32_t heldBar_ = 0;
};

/// The board's one song transport, and where its playhead stands: Arrange
/// claims it, and the seq view opened from the MODE map reads which block of
/// the seq track the playhead is on. Header-only, as arrangeFocus() is.
inline SongTransport& songTransport()
{
    static SongTransport transport;
    return transport;
}

} // namespace crosspad

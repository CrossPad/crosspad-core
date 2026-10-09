// SPDX-License-Identifier: MIT
#pragma once

#include "crosspad/platform/PlatformServices.hpp"
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
    /// it -- or from the loop region's start while looping one.
    void play() override
    {
        ISongEngine* se = engine();
        if (!se || se->playing()) return;
        uint32_t from = heldBar_;
        {
            std::lock_guard<std::recursive_mutex> l(se->songMutex());
            const AudioSong& song = se->song();
            if (song.loop && song.hasRegion()) from = song.loopStart;
        }
        se->play(from);
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

    /// A counted-in start is armed (amber on the bar) until its beat one.
    TransportState state() const override
    {
        ISongEngine* se = engine();
        if (!se || !se->playing()) return TransportState::Stopped;
        return se->countInBeatsLeft() ? TransportState::Waiting : TransportState::Playing;
    }

    /// A song is arranged, not recorded into: REC and DUB grey out rather
    /// than offering something the engine cannot do. Takes are the
    /// sequencer's, and bouncing one in is the Sequencer's BOUNCE.
    uint8_t caps() const override
    {
        switch (state()) {
            case TransportState::Waiting: return kTransportCanStop;
            case TransportState::Stopped: return kTransportCanPlay | kTransportCanStop | kTransportCanLocate;
            default:                      return kTransportCanPlay | kTransportCanStop;
        }
    }

    /// The bar's position turned while stopped: a bar a detent, never before
    /// the song's bar one.
    void locate(int bars) override
    {
        const int64_t to = (int64_t)heldBar_ + bars;
        heldBar_ = (uint32_t)(to < 0 ? 0 : (to > kMaxBar ? kMaxBar : to));
    }

    /// "ARR": the block song of the Song screen is "SONG" (UX 2000 3.2).
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
    /// Arrange's last bar (kMaxLength), 0-based.
    static constexpr int64_t kMaxBar = 998;

    uint32_t heldBar_ = 0;
};

} // namespace crosspad

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
    /// Where PLAY starts, when no loop region says otherwise: the bar the
    /// screen is working on. Arrange sets it from its selection, so the key
    /// in the bar starts where its own PLAY button used to -- that rule was
    /// the button's and would have been lost with it.
    void setStartBar(uint32_t bar) { startBar_ = bar; }

    void play() override
    {
        ISongEngine* se = engine();
        if (!se) return;
        uint32_t from = startBar_;
        {
            std::lock_guard<std::recursive_mutex> l(se->songMutex());
            const AudioSong& song = se->song();
            /* Looping a region: from its start, the place being worked on. */
            if (song.loop && song.hasRegion()) from = song.loopStart;
        }
        se->play(from);
    }

    void stop() override
    {
        if (ISongEngine* se = engine()) se->stop();
    }

    TransportState state() const override
    {
        ISongEngine* se = engine();
        return (se && se->playing()) ? TransportState::Playing : TransportState::Stopped;
    }

    /// A song is arranged, not recorded into: REC and DUB grey out rather
    /// than offering something the engine cannot do. Takes are the
    /// sequencer's, and bouncing one in is the Sequencer's BOUNCE.
    uint8_t caps() const override { return kTransportCanPlay | kTransportCanStop; }

    const char* name() const override { return "SONG"; }

    float bpm() const override
    {
        ISongEngine* se = engine();
        if (!se) return 0.0f;
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        return se->song().bpm;
    }

    /// The engine counts in bars, fractionally; the bar's readout wants a
    /// bar, a beat and hundredths of one. Four beats to the bar, which is
    /// what AudioSong's own metre assumes.
    TransportPos position() const override
    {
        TransportPos p;
        ISongEngine* se = engine();
        if (!se) return p;
        double bars = se->positionBars();
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

    uint32_t startBar_ = 0;
};

} // namespace crosspad

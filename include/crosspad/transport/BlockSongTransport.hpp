// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/sequencer/SequencerController.hpp"
#include "crosspad/sequencer/SequencerTypes.hpp"
#include "crosspad/transport/Transport.hpp"

/**
 * @file BlockSongTransport.hpp
 * @brief The Song app's song -- scenes in blocks -- as something the bar drives.
 *
 * Not the arranged audio song (SongTransport, Arrange's): this one is the
 * sequencer's own song mode, a row of "scene x repeats" blocks it walks on
 * the bar line. The Song screen claims it, so PLAY on the bar plays the song
 * and not the scene that happens to be selected (UX 2000 section 3.2).
 *
 * A song is arranged, not recorded into: REC and DUB are greyed, and the
 * takes are made in the Sequencer.
 */

namespace crosspad {

class BlockSongTransport final : public ITransportTarget {
public:
    explicit BlockSongTransport(SequencerController& seq) : seq_(seq) {}

    /// Locate: the block the next PLAY starts on. The Song screen sets it from
    /// its selection.
    void setStartBlock(uint8_t block) { heldBlock_ = block; }

    /// From a stop, from the held block -- where the last stop or a locate
    /// left it -- and into it as far as the clock was.
    void play() override
    {
        if (state() != TransportState::Stopped || !seq_.getSongLength()) return;
        if (heldBlock_ >= seq_.getSongLength()) heldBlock_ = 0;
        /* Song mode first: switched on while stopped it locates the first
         * block, which would undo the locate. */
        seq_.setSongMode(true);
        seq_.songLocate(heldBlock_);
        seq_.playback();
    }

    /// Stops in the block it was in; stopped already, back to the top.
    void stop() override
    {
        if (state() == TransportState::Stopped) {
            heldBlock_ = 0;
            seq_.rewind();
            return;
        }
        if (seq_.getSongMode()) heldBlock_ = seq_.getSongPosition();
        seq_.stop();
    }

    TransportState state() const override
    {
        switch (seq_.getState()) {
            case SequencerState::Idle:    return TransportState::Stopped;
            case SequencerState::Running: return TransportState::Playing;
            default:                      return TransportState::Waiting;
        }
    }

    uint8_t caps() const override
    {
        if (state() == TransportState::Waiting) return kTransportCanStop;
        return seq_.getSongLength() ? (kTransportCanPlay | kTransportCanStop) : kTransportCanStop;
    }

    const char* name() const override { return "SONG"; }

    float bpm() const override { return seq_.getBpm(); }

    /// Bars from the top of the song: the blocks before this one, the passes
    /// of this one already played, and how far into the scene the clock is.
    /// Stopped, the start of the held block.
    TransportPos position() const override
    {
        TransportPos p;
        const bool running = state() == TransportState::Playing && seq_.getSongMode();
        const uint8_t at = running ? seq_.getSongPosition() : heldBlock_;
        float bars = 0.0f;
        for (uint8_t i = 0; i < seq_.getSongLength() && i < at; ++i) {
            uint8_t scene = 0, repeats = 1;
            if (!seq_.getSongBlock(i, scene, repeats)) break;
            const uint8_t b = seq_.getSceneBars(scene);
            bars += (float)repeats * (float)(b ? b : 1);
        }
        if (running) {
            uint8_t scene = 0, repeats = 1;
            if (seq_.getSongBlock(at, scene, repeats)) {
                const uint8_t b = seq_.getSceneBars(scene) ? seq_.getSceneBars(scene) : 1;
                bars += (float)seq_.getSongPass() * b + seq_.getProgress() * b;
            }
        }
        const uint32_t whole = (uint32_t)bars;
        const float beats = (bars - (float)whole) * 4.0f;
        const uint32_t beat = (uint32_t)beats;
        p.bar  = (uint16_t)(whole + 1u);
        p.beat = (uint8_t)(beat + 1u);
        p.cent = (uint8_t)((beats - (float)beat) * 100.0f);
        if (p.cent > 99) p.cent = 99;
        return p;
    }

private:
    SequencerController& seq_;
    uint8_t heldBlock_ = 0;
};

/// The board's one block-song transport, bound like sequencerTransport().
/// Null until the platform has published a SequencerController.
BlockSongTransport* blockSongTransport();

} // namespace crosspad

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/sampletools/RecInput.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace crosspad {

/// Arrange's REC onto an armed audio track: the track's input recorded over
/// the song and laid on the track where it was sung, with no other screen.
///
/// The recording is the sample tools' take lined up with the song
/// (ISampleTools::startRecordingWithSong): the song plays from the bar the
/// take starts on, after the count-in, and frame 0 of the take is that bar,
/// the codecs' delay taken off. A song with nothing in it yet has nothing to
/// line up with: the take then runs on its own and lands on the bar it was
/// started from. Stopped, the take is put on the track as one block from its
/// bar (ISongEngine::requestImport) -- what was under it on that track goes,
/// as with the Recorder's ARR. The WAV stays in the recordings folder too.
///
/// With LOOP on the song goes round under the take -- its region, or the
/// whole song -- and the take is cut into its passes: every complete one is
/// a clip of its own in the song's clips/, laid on the track in turn, so the
/// last one is on the track and the others are a pick away (EDIT, a clip
/// from the library). A pass is complete when the song got round: the first
/// from the bar the take went in at to the loop's end, each after it the
/// whole loop. What was sung after the last complete pass is not kept;
/// stopped before the first got round, the take is laid as it is.
///
/// SongTransport drives it (REC, STOP, PLAY while a take runs); Arrange's
/// timer calls poll(), which is what lands a take that has ended.
class ArrangeAudioTake {
public:
    /// What poll() has to say about a take that has just been dealt with.
    struct Result {
        bool        done = false;   ///< a take ended since the last poll
        bool        ok   = false;   ///< it is on its track
        int         track = -1;
        uint32_t    bar  = 0;       ///< 0-based, where the one on the track landed
        uint32_t    passes = 0;     ///< clips made: the loop's complete passes, else 1
        std::string error;          ///< why it is not, when it is not
    };

    /// Start recording the armed audio track's input from @p fromBar after
    /// @p countInBeats. With the song already playing the take goes in at its
    /// next bar line. False, with lastError() saying why, when there is no
    /// armed audio track, its input cannot record now, or a take runs.
    bool start(uint32_t fromBar, uint8_t countInBeats);
    /// End the take (and the song, for one lined up with it).
    void stop();
    /// A take is being recorded (or counted in).
    bool recording() const { return phase_ == Phase::Recording; }
    /// Counted in, or armed and waiting for the song to sound.
    bool waiting() const;
    /// The track the take is for, while one runs or lands; -1 otherwise.
    int  track() const { return phase_ == Phase::Idle ? -1 : track_; }
    /// Lands a take that has ended. Cheap while nothing has.
    Result poll();
    const std::string& lastError() const { return error_; }
    /// Counts the starts refused: a REC pressed anywhere (the top bar, the
    /// MODE map) that a screen should say something about.
    uint32_t refusals() const { return refusals_; }

private:
    enum class Phase : uint8_t { Idle, Recording, Landing, Importing };

    /// A stretch of the take's file and the bar it lands on.
    struct Piece { uint32_t start = 0, end = 0, bar = 0; };

    Result finish(bool ok, const std::string& error);
    /// The take cut into what lands: its loop passes, or all of it.
    void plan(uint32_t fromBar, uint32_t frames, uint32_t framesPerBar);
    /// The next piece to the song; false while the song is busy.
    bool importNext();

    Phase       phase_ = Phase::Idle;
    int         track_ = -1;
    uint32_t    fromBar_ = 0;
    uint32_t    landBar_ = 0;
    uint32_t    landTries_ = 0;
    /* The loop the song went round, taken when the take started: [loopA_,
     * loopB_) in bars; loopB_ 0 = it does not loop. */
    uint32_t    loopA_ = 0;
    uint32_t    loopB_ = 0;
    std::string file_;
    std::vector<Piece> pieces_;
    size_t      next_ = 0;
    std::string error_;
    uint32_t    refusals_ = 0;
};

/// The board's one audio take in Arrange.
ArrangeAudioTake& arrangeAudioTake();

} // namespace crosspad

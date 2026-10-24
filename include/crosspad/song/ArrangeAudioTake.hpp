// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/sampletools/RecInput.hpp"

#include <cstdint>
#include <string>

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
/// SongTransport drives it (REC, STOP, PLAY while a take runs); Arrange's
/// timer calls poll(), which is what lands a take that has ended.
class ArrangeAudioTake {
public:
    /// What poll() has to say about a take that has just been dealt with.
    struct Result {
        bool        done = false;   ///< a take ended since the last poll
        bool        ok   = false;   ///< it is on its track
        int         track = -1;
        uint32_t    bar  = 0;       ///< 0-based, where it landed
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

private:
    enum class Phase : uint8_t { Idle, Recording, Landing, Importing };

    Result finish(bool ok, const std::string& error);

    Phase       phase_ = Phase::Idle;
    int         track_ = -1;
    uint32_t    fromBar_ = 0;
    uint32_t    landBar_ = 0;
    uint32_t    landTries_ = 0;
    std::string error_;
};

/// The board's one audio take in Arrange.
ArrangeAudioTake& arrangeAudioTake();

} // namespace crosspad

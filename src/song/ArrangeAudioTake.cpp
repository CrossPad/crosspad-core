// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/song/ArrangeAudioTake.hpp"

#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sampletools/ISampleTools.hpp"
#include "crosspad/song/AudioSong.hpp"
#include "crosspad/song/ISongEngine.hpp"

#include <mutex>

namespace crosspad {

namespace {

/// Polls a finished take is offered to the song before it is given up: the
/// song engine refuses an edit while the song it stopped is still winding
/// down, or another edit runs. Arrange polls every ~50 ms.
constexpr uint32_t kLandTries = 200;

ISongEngine*  engine() { return getPlatformServices().songEngine; }
ISampleTools* tools()  { return getPlatformServices().sampleTools; }

} // namespace

bool ArrangeAudioTake::start(uint32_t fromBar, uint8_t countInBeats)
{
    error_.clear();
    ISongEngine* se = engine();
    ISampleTools* st = tools();
    if (!se || !st) { error_ = "no recorder in this firmware"; return false; }
    if (phase_ != Phase::Idle || st->recStatus().recording) { error_ = "a take is running"; return false; }
    int track = -1;
    RecInput input = RecInput::Mic;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const AudioSong& song = se->song();
        track = song.armedAudioTrack();
        if (track >= 0) input = song.tracks[track].input;
    }
    if (track < 0) { error_ = "no audio track is armed"; return false; }
    if (!st->inputAvailable(input)) {
        error_ = input == RecInput::Usb ? "USB records what the computer plays: switch Settings > USB to audio"
                                        : "this input cannot record now";
        return false;
    }
    if (!st->selectInput(input)) { error_ = "the input cannot be switched now"; return false; }
    /* An empty song has nothing to line the take up with: it runs on its own
     * and lands on the bar it was started from. */
    const bool withSong = se->playing() || se->playEndBars() > 0;
    const bool ok = withSong ? st->startRecordingWithSong(fromBar, countInBeats) : st->startRecording();
    if (!ok) {
        st->releaseInput();
        error_ = "cannot record now";
        return false;
    }
    track_ = track;
    fromBar_ = fromBar;
    phase_ = Phase::Recording;
    return true;
}

void ArrangeAudioTake::stop()
{
    if (phase_ != Phase::Recording) return;
    if (ISampleTools* st = tools()) st->stopRecording();
    phase_ = Phase::Landing;
    landTries_ = 0;
}

bool ArrangeAudioTake::waiting() const
{
    if (phase_ != Phase::Recording) return false;
    ISampleTools* st = tools();
    if (st && st->recStatus().waiting) return true;
    ISongEngine* se = engine();
    return se && se->countInBeatsLeft() > 0;
}

ArrangeAudioTake::Result ArrangeAudioTake::finish(bool ok, const std::string& error)
{
    Result r;
    r.done = true;
    r.ok = ok;
    r.track = track_;
    r.bar = landBar_;
    r.error = error;
    if (ISampleTools* st = tools()) st->releaseInput();
    phase_ = Phase::Idle;
    track_ = -1;
    return r;
}

ArrangeAudioTake::Result ArrangeAudioTake::poll()
{
    ISongEngine* se = engine();
    ISampleTools* st = tools();
    if (phase_ == Phase::Idle) return {};
    if (!se || !st) return finish(false, "no recorder in this firmware");

    if (phase_ == Phase::Recording) {
        /* Ended by itself: the song ran out, the card filled, or the take's
         * memory was taken back. */
        if (st->recStatus().recording) return {};
        phase_ = Phase::Landing;
        landTries_ = 0;
    }
    if (phase_ == Phase::Landing) {
        const RecStatus rs = st->recStatus();
        if (rs.recording) return {};                 // its file is still being closed
        if (!rs.error.empty()) return finish(false, rs.error);
        if (!rs.frames || rs.file.empty()) return finish(false, "nothing was recorded");
        landBar_ = rs.withSong ? rs.fromBar : fromBar_;
        if (se->requestImport(rs.file, 0, 0, (uint8_t)track_, landBar_, 0)) {
            phase_ = Phase::Importing;
            return {};
        }
        if (++landTries_ < kLandTries) return {};
        return finish(false, "the song stayed busy: the take is in the recordings folder");
    }
    switch (se->editState()) {
        case SongOpState::Busy:  return {};
        case SongOpState::Ready: return finish(true, {});
        default:                 return finish(false, "the take could not be put in the song: it is in the recordings folder");
    }
}

ArrangeAudioTake& arrangeAudioTake()
{
    static ArrangeAudioTake take;
    return take;
}

} // namespace crosspad

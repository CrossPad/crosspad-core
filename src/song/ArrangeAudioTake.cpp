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
    auto refuse = [this](const char* why) {
        error_ = why;
        ++refusals_;
        return false;
    };
    ISongEngine* se = engine();
    ISampleTools* st = tools();
    if (!se || !st) return refuse("no recorder in this firmware");
    if (phase_ != Phase::Idle || st->recStatus().recording) return refuse("a take is running");
    int track = -1;
    RecInput input = RecInput::Mic;
    uint32_t loopA = 0, loopB = 0;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const AudioSong& song = se->song();
        track = song.armedAudioTrack();
        if (track >= 0) input = song.tracks[track].input;
        if (song.loop) {
            loopA = song.hasRegion() ? song.loopStart : 0;
            loopB = song.hasRegion() ? song.loopEnd : se->playEndBars();
        }
    }
    if (track < 0) return refuse("no audio track is armed");
    if (!st->inputAvailable(input)) {
        return refuse(input == RecInput::Usb ? "USB records what the computer plays: switch Settings > USB to audio"
                                             : "this input cannot record now");
    }
    if (!st->selectInput(input)) return refuse("the input cannot be switched now");
    /* An empty song has nothing to line the take up with: it runs on its own
     * and lands on the bar it was started from. */
    const bool withSong = se->playing() || se->playEndBars() > 0;
    const bool ok = withSong ? st->startRecordingWithSong(fromBar, countInBeats) : st->startRecording();
    if (!ok) {
        st->releaseInput();
        return refuse("cannot record now");
    }
    track_ = track;
    fromBar_ = fromBar;
    loopA_ = withSong ? loopA : 0;
    loopB_ = withSong ? loopB : 0;
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
    r.passes = ok ? (uint32_t)pieces_.size() : 0;
    r.error = error;
    if (ISampleTools* st = tools()) st->releaseInput();
    phase_ = Phase::Idle;
    track_ = -1;
    pieces_.clear();
    return r;
}

void ArrangeAudioTake::plan(uint32_t fromBar, uint32_t frames, uint32_t framesPerBar)
{
    pieces_.clear();
    next_ = 0;
    /* A pass is complete once the song got round: the first from fromBar to
     * the loop's end, every one after it the whole loop. */
    if (loopB_ > loopA_ && fromBar < loopB_ && framesPerBar) {
        const uint64_t first = (uint64_t)(loopB_ - fromBar) * framesPerBar;
        const uint64_t lap = (uint64_t)(loopB_ - loopA_) * framesPerBar;
        if (frames >= first) {
            pieces_.push_back({0, (uint32_t)first, fromBar});
            for (uint64_t at = first; at + lap <= frames; at += lap)
                pieces_.push_back({(uint32_t)at, (uint32_t)(at + lap), loopA_});
        }
    }
    if (pieces_.empty()) pieces_.push_back({0, 0, fromBar});   // all of it, where it went in
}

bool ArrangeAudioTake::importNext()
{
    ISongEngine* se = engine();
    const Piece& pc = pieces_[next_];
    if (!se || !se->requestImport(file_, pc.start, pc.end, (uint8_t)track_, pc.bar, 0)) return false;
    landBar_ = pc.bar;
    ++next_;
    return true;
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
        if (pieces_.empty()) {
            file_ = rs.file;
            plan(rs.withSong ? rs.fromBar : fromBar_, rs.frames, se->framesPerBar());
        }
        if (importNext()) {
            phase_ = Phase::Importing;
            landTries_ = 0;
            return {};
        }
        if (++landTries_ < kLandTries) return {};
        return finish(false, "the song stayed busy: the take is in the recordings folder");
    }
    switch (se->editState()) {
        case SongOpState::Busy:  return {};
        case SongOpState::Ready: break;
        default:                 return finish(false, "the take could not be put in the song: it is in the recordings folder");
    }
    if (next_ >= pieces_.size()) return finish(true, {});
    /* The next pass goes over the one just laid; that one stays in clips/. */
    if (importNext()) { landTries_ = 0; return {}; }
    if (++landTries_ < kLandTries) return {};
    return finish(false, "the song stayed busy: the passes not laid are in the recording");
}

ArrangeAudioTake& arrangeAudioTake()
{
    static ArrangeAudioTake take;
    return take;
}

} // namespace crosspad

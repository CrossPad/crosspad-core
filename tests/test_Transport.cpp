// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sequencer/SequencerController.hpp"
#include "crosspad/transport/Transport.hpp"

#include <string>

using crosspad::ITransportTarget;
using crosspad::TransportState;

namespace {

/// A target that records what it was asked, so the rules can be checked
/// rather than assumed.
class Fake : public ITransportTarget {
public:
    Fake(const char* n, uint8_t caps) : name_(n), caps_(caps) {}

    void play() override    { ++plays;    st_ = TransportState::Playing; }
    void stop() override    { ++stops;    st_ = TransportState::Stopped; }
    void record() override  { ++records;  st_ = TransportState::Recording; }
    void overdub() override { ++overdubs; st_ = TransportState::Overdubbing; }

    TransportState state() const override { return st_; }
    uint8_t caps() const override { return caps_; }
    const char* name() const override { return name_.c_str(); }

    int plays = 0, stops = 0, records = 0, overdubs = 0;

private:
    std::string name_;
    uint8_t caps_;
    TransportState st_ = TransportState::Stopped;
};

constexpr uint8_t kAll = crosspad::kTransportCanPlay | crosspad::kTransportCanStop |
                         crosspad::kTransportCanRecord | crosspad::kTransportCanOverdub;

} // namespace

TEST_CASE("Transport with no target is stopped and ignores every command", "[transport]") {
    crosspad::transportClaim(nullptr);
    REQUIRE(crosspad::transportTarget() == nullptr);
    REQUIRE(crosspad::transportState() == TransportState::Stopped);
    REQUIRE(crosspad::transportCaps() == 0);
    REQUIRE(std::string(crosspad::transportName()).empty());
    /* A bar may press these before anything has claimed. */
    crosspad::transportPlay();
    crosspad::transportStop();
    crosspad::transportRecord();
    crosspad::transportOverdub();
}

TEST_CASE("Transport sends each command to the holder", "[transport]") {
    Fake seq("PATTERN", kAll);
    crosspad::transportClaim(&seq);
    REQUIRE(std::string(crosspad::transportName()) == "PATTERN");

    crosspad::transportPlay();
    REQUIRE(seq.plays == 1);
    REQUIRE(crosspad::transportState() == TransportState::Playing);

    crosspad::transportRecord();
    REQUIRE(seq.records == 1);
    REQUIRE(crosspad::transportState() == TransportState::Recording);

    crosspad::transportOverdub();
    REQUIRE(seq.overdubs == 1);

    crosspad::transportStop();
    REQUIRE(seq.stops == 1);
    REQUIRE(crosspad::transportState() == TransportState::Stopped);

    crosspad::transportRelease(&seq);
    REQUIRE(crosspad::transportTarget() == nullptr);
}

TEST_CASE("A command a target cannot do is not sent at all", "[transport]") {
    /* The audio song plays and stops; it does not record or overdub. A bar
     * greys those, and must be able to press them harmlessly anyway. */
    Fake song("SONG", crosspad::kTransportCanPlay | crosspad::kTransportCanStop);
    crosspad::transportClaim(&song);

    crosspad::transportRecord();
    crosspad::transportOverdub();
    REQUIRE(song.records == 0);
    REQUIRE(song.overdubs == 0);

    crosspad::transportPlay();
    REQUIRE(song.plays == 1);
    crosspad::transportRelease(&song);
}

TEST_CASE("A claim stops whoever held it", "[transport]") {
    /* One transport means one thing sounding: taking it is how an app says
     * "PLAY now means me", and the pattern must not keep running underneath
     * the song that just took over. */
    Fake seq("PATTERN", kAll);
    Fake song("SONG", kAll);

    crosspad::transportClaim(&seq);
    crosspad::transportPlay();
    REQUIRE(seq.plays == 1);
    REQUIRE(seq.stops == 0);

    crosspad::transportClaim(&song);
    REQUIRE(seq.stops == 1);                  // stopped by the handover
    REQUIRE(crosspad::transportTarget() == &song);
    REQUIRE(crosspad::transportState() == TransportState::Stopped);

    crosspad::transportPlay();
    REQUIRE(song.plays == 1);
    REQUIRE(seq.plays == 1);                  // and not twice

    /* Claiming it again changes nothing -- an app may claim on every show. */
    crosspad::transportClaim(&song);
    REQUIRE(song.stops == 0);

    crosspad::transportRelease(&song);
}

TEST_CASE("Releasing a target that is not holding it leaves the holder alone",
          "[transport]") {
    Fake a("A", kAll), b("B", kAll);
    crosspad::transportClaim(&a);
    crosspad::transportRelease(&b);           // b never had it
    REQUIRE(crosspad::transportTarget() == &a);
    crosspad::transportRelease(&a);
    REQUIRE(crosspad::transportTarget() == nullptr);
}

namespace {
/// A target with a position of its own that the bar may move.
class Locatable : public Fake {
public:
    Locatable() : Fake("ARR", crosspad::kTransportCanPlay | crosspad::kTransportCanStop |
                                  crosspad::kTransportCanLocate) {}
    void locate(int bars) override { moved += bars; }
    int moved = 0;
};
} // namespace

TEST_CASE("Transport locates only a stopped holder that can", "[transport][locate]") {
    Locatable arr;
    crosspad::transportClaim(&arr);
    crosspad::transportLocate(3);
    crosspad::transportLocate(-1);
    REQUIRE(arr.moved == 2);
    crosspad::transportPlay();
    crosspad::transportLocate(4);
    REQUIRE(arr.moved == 2);   // playing: the position is the song's

    Fake seq("PATTERN", kAll);
    crosspad::transportClaim(&seq);
    crosspad::transportLocate(1);   // cannot: nothing happens
    crosspad::transportClaim(nullptr);
}

TEST_CASE("Transport tempo is the sequencer's, clamped, and follows the host flag", "[transport][tempo]") {
    crosspad::SequencerController ctrl;
    crosspad::getPlatformServices().sequencer = &ctrl;
    crosspad::transportSetTempo(500.0f);
    REQUIRE(crosspad::transportTempo() == crosspad::kTransportTempoMax);
    crosspad::transportSetTempo(92.0f);
    REQUIRE(crosspad::transportTempo() == 92.0f);
    crosspad::transportCommitTempo();   // no song engine: the clock only
    REQUIRE(crosspad::transportTempo() == 92.0f);

    REQUIRE_FALSE(crosspad::transportTempoFromHost());
    crosspad::transportSetTempoFromHost(true);
    REQUIRE(crosspad::transportTempoFromHost());
    crosspad::transportSetTempoFromHost(false);
    crosspad::getPlatformServices().sequencer = nullptr;
    REQUIRE(crosspad::transportTempo() == 0.0f);
}

TEST_CASE("Transport metronome is one switch no command flips", "[transport][click]") {
    crosspad::SequencerController ctrl;
    crosspad::getPlatformServices().sequencer = &ctrl;
    crosspad::transportSetClick(false);
    REQUIRE_FALSE(crosspad::transportClick());
    ctrl.record();
    REQUIRE_FALSE(crosspad::transportClick());   // a take does not turn it on
    crosspad::transportSetClick(true);
    ctrl.stop();
    REQUIRE(crosspad::transportClick());         // nor a stop off
    crosspad::getPlatformServices().sequencer = nullptr;
    REQUIRE_FALSE(crosspad::transportClick());
}

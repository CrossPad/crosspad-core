// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/pad/IPadMix.hpp"
#include "crosspad/song/ISongEngine.hpp"

using namespace crosspad;

namespace {
// The smallest engine an app can be written against: every card operation
// is a request, answered later through state and a generation counter.
class FakeSongEngine : public ISongEngine {
public:
    bool requestProjects() override { ++gen_; return true; }
    std::vector<std::string> projects() override { return {"a", "b"}; }
    bool requestOpen(const std::string& n) override { song_.name = n; state_ = SongOpState::Ready; return true; }
    SongOpState openState() const override { return state_; }
    AudioSong& song() override { return song_; }
    std::recursive_mutex& songMutex() override { return m_; }
    void commit() override {}
    bool requestClips() override { ++gen_; return true; }
    std::vector<ClipInfo> clips() override { return {}; }
    uint32_t listGeneration() const override { return gen_; }
    bool bounceScene(uint8_t, uint8_t) override { return false; }
    BounceStatus bounceStatus() override { return {}; }
    void cancelBounce() override {}
    bool play(uint32_t) override { return false; }
    void stop() override {}
    bool playing() const override { return false; }
    double positionBars() const override { return 0.0; }
    uint32_t underruns() const override { return 0; }
    uint32_t openFailures() const override { return 0; }

private:
    AudioSong song_;
    std::recursive_mutex m_;
    SongOpState state_ = SongOpState::Idle;
    uint32_t gen_ = 0;
};

class FakePadMix : public IPadMix {
public:
    void setPadMix(uint8_t pad, int volume, int pan) override { last = {pad, volume, pan}; }
    struct { uint8_t pad; int volume; int pan; } last{};
};
} // namespace

TEST_CASE("PlatformServices: no song engine and no pad mix until a platform sets them", "[platform]") {
    PlatformServices s;
    REQUIRE(s.songEngine == nullptr);
    REQUIRE(s.padMix == nullptr);
}

TEST_CASE("ISongEngine: requests return at once, results come through state", "[platform][song]") {
    FakeSongEngine e;
    const uint32_t g = e.listGeneration();
    REQUIRE(e.requestProjects());
    REQUIRE(e.listGeneration() != g);
    REQUIRE(e.requestOpen("demo"));
    REQUIRE(e.openState() == SongOpState::Ready);
    std::lock_guard<std::recursive_mutex> lock(e.songMutex());
    REQUIRE(e.song().name == "demo");
}

TEST_CASE("IPadMix: an app sets a pad's level through the interface", "[platform]") {
    FakePadMix m;
    PlatformServices s;
    s.padMix = &m;
    s.padMix->setPadMix(4, 80, -20);
    REQUIRE(m.last.pad == 4);
    REQUIRE(m.last.volume == 80);
    REQUIRE(m.last.pan == -20);
}

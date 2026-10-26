// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/instrument/IInstrumentHost.hpp"
#include "crosspad/kit/PortableKitLoader.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/song/ISongEngine.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace test {

/// A song engine whose open song a test writes straight into.
class FakeSongEngine final : public crosspad::ISongEngine {
public:
    bool requestProjects() override { return true; }
    std::vector<std::string> projects() override { return {}; }
    bool requestOpen(const std::string& n) override { song_.name = n; return true; }
    crosspad::SongOpState openState() const override { return state; }
    crosspad::AudioSong& song() override { return song_; }
    std::recursive_mutex& songMutex() override { return m_; }
    void commit() override { ++commits; }
    bool requestClips() override { return true; }
    std::vector<crosspad::ClipInfo> clips() override { return {}; }
    uint32_t listGeneration() const override { return 0; }
    bool bounceScene(uint8_t, uint8_t) override { return false; }
    crosspad::BounceStatus bounceStatus() override { return bounce; }
    void cancelBounce() override {}
    bool play(uint32_t) override { return false; }
    void stop() override {}
    bool playing() const override { return false; }
    double positionBars() const override { return 0.0; }
    uint32_t underruns() const override { return 0; }
    uint32_t openFailures() const override { return 0; }

    /// Track 0 a seq track playing @p source.
    void addSeqTrack(crosspad::SeqSource source) {
        song_.tracks[0].inUse = true;
        song_.tracks[0].kind = crosspad::TrackKind::Seq;
        song_.tracks[0].source = source;
    }

    crosspad::SongOpState state = crosspad::SongOpState::Ready;
    crosspad::BounceStatus bounce;
    int commits = 0;

private:
    crosspad::AudioSong song_;
    std::recursive_mutex m_;
};

/// An instrument host that does what it is told when it is told: a test sets
/// its state and reads what was asked of it.
class FakeInstrumentHost final : public crosspad::IInstrumentHost {
public:
    bool requestLoad(const crosspad::KitInfo&) override { return true; }
    bool requestOpen(const std::string& path) override { opened.push_back(path); return true; }
    bool requestSave(const crosspad::KitInfo&) override { return true; }
    void release() override { ++releases; }
    State state() const override { return st; }
    crosspad::KitInfo instrument() const override { return inst; }
    std::string message() const override { return msg; }
    std::string libraryRoot() const override { return "/crosspad/pitchplay"; }

    State st = State::Idle;
    std::string msg;
    crosspad::KitInfo inst;
    std::vector<std::string> opened;
    int releases = 0;
};

/// Publishes the given services for one test and takes them back after.
struct ServicesScope {
    crosspad::ISongEngine* songEngine;
    crosspad::IKitManager* kitManager;
    crosspad::IInstrumentHost* instrumentHost;

    ServicesScope(crosspad::ISongEngine* se, crosspad::IKitManager* kits, crosspad::IInstrumentHost* host) {
        auto& ps = crosspad::getPlatformServices();
        songEngine = ps.songEngine;
        kitManager = ps.kitManager;
        instrumentHost = ps.instrumentHost;
        ps.songEngine = se;
        ps.kitManager = kits;
        ps.instrumentHost = host;
    }
    ~ServicesScope() {
        auto& ps = crosspad::getPlatformServices();
        ps.songEngine = songEngine;
        ps.kitManager = kitManager;
        ps.instrumentHost = instrumentHost;
    }
};

/// A kit list of the given kit.json paths, none current.
inline void setKits(crosspad::PortableKitLoader& kits, const std::vector<std::string>& paths) {
    kits.kits().clear();
    for (const auto& p : paths) {
        crosspad::KitInfo k;
        k.path = p;
        k.name = p.substr(0, p.rfind('/'));
        kits.kits().push_back(k);
    }
    kits.setCurrentKitId(-1);
}

} // namespace test

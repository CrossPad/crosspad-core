// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/song/SongSounds.hpp"

#include "crosspad/instrument/IInstrumentHost.hpp"
#include "crosspad/instrument/InstrumentPlayState.hpp"
#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/song/ISongEngine.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

namespace crosspad {

namespace {

/// The longest warning the player is shown, terminator included.
constexpr size_t kWarnChars = 96;

ISongEngine* songs() { return getPlatformServices().songEngine; }

int findKit(IKitManager& kits, const std::string& path)
{
    for (int i = 0; i < kits.getKitCount(); ++i) {
        const KitInfo* k = kits.getKit(i);
        if (k && k->path == path) return i;
    }
    return -1;
}

} // namespace

/// A song's folder as the card's root names it: "/SONGS/<name>".
std::string SongSounds::logical(const std::string& dir)
{
    const std::string mount = host_.cardRoot();
    return dir.compare(0, mount.size(), mount) == 0 ? dir.substr(mount.size()) : dir;
}

bool SongSounds::foregroundIs(const char* a, const char* b)
{
    const char* name = host_.foregroundApp();
    return name && (std::strcmp(name, a) == 0 || (b && std::strcmp(name, b) == 0));
}

/// No kit current. An instrument holding the engine is the platform's to
/// leave alone.
void SongSounds::unloadKit()
{
    IKitManager* kits = getKitManager();
    KitInfo* cur = kits ? kits->getCurrentKit() : nullptr;
    if (!cur) return;
    host_.log(false, ("the song has no kit: giving back " + cur->name).c_str());
    host_.unloadKit();
}

void SongSounds::start()
{
    pending_.store(true);
}

void SongSounds::songOpened()
{
    openGen_.fetch_add(1);
    pending_.store(true, std::memory_order_release);
}

void SongSounds::step()
{
    if (!pending_.load(std::memory_order_acquire)) return;
    ISongEngine* se = songs();
    IKitManager* kits = getKitManager();
    if (!se || !kits) { pending_.store(false); return; }
    if (se->openState() != SongOpState::Ready) return;
    if (!host_.kitsSettled()) return;
    IInstrumentHost* host = getPlatformServices().instrumentHost;
    if (host && host->state() == IInstrumentHost::State::Busy) return;

    std::string kit, inst;
    bool pitch = false;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const AudioSong& song = se->song();
        kit = song.kit;
        pitch = song.hasPitchTrack();
        /* AudioSong::instrument as a path from the card's root. */
        inst = song.instrument.empty() || song.instrument[0] == '/'
             ? song.instrument : logical(song.dir) + "/" + song.instrument;
    }
    const bool wantInst = pitch && !inst.empty();

    /* No instrument in this song: the kits' engine first. Giving the
     * instrument back reloads the kit, and the kit is compared after that.
     * Not out from under Pitch Play or the Sequencer playing it. */
    if (!wantInst && host && host_.instrumentOwnsEngine() && !foregroundIs("PitchPlay", "Sequencer")) {
        host->release();
        return;
    }

    KitInfo* cur = kits->getCurrentKit();
    const std::string have = cur ? cur->path : std::string();
    if (kit != have) {
        if (!kit.empty()) {
            const int id = findKit(*kits, kit);
            if (id >= 0) {
                host_.log(false, ("the song's kit: " + kit).c_str());
                host_.loadKit(id);
                return;
            }
            const uint32_t gen = openGen_.load();
            if (gen != missingSaid_) {
                missingSaid_ = gen;
                host_.log(true, ("the song's kit is not on the card: " + kit).c_str());
                char msg[kWarnChars];
                std::snprintf(msg, sizeof(msg), "This song's kit is not on the card: %s", kit.c_str());
                host_.warn(msg);
            }
        }
        /* None in the song, or not on the card: none in memory either. Not
         * under the Sampler, which reads the kit it shows. */
        if (foregroundIs("Sampler")) return;
        unloadKit();
    }

    if (wantInst && host) {
        const bool loaded = host->state() == IInstrumentHost::State::Ready &&
                            host->instrument().path == inst;
        if (!loaded) {
            host_.log(false, ("the song's instrument: " + inst).c_str());
            host->requestOpen(inst);
            return;
        }
        /* What Arrange keeps loaded for the Pitch track, and what the
         * Sequencer and the Recorder borrow: this song's. */
        InstrumentPlayState& ps = instrumentPlayState();
        if (ps.instrumentPath != inst) {
            ps.instrumentPath = inst;
            ps.instrumentName = host->instrument().name;
            instrumentPlayStatePublished();
        }
    }
    pending_.store(false);
}

void SongSounds::kitLoaded()
{
    ISongEngine* se = songs();
    IKitManager* kits = getKitManager();
    if (!se || !kits || se->openState() != SongOpState::Ready) return;
    const KitInfo* cur = kits->getCurrentKit();
    if (!cur) return;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        AudioSong& song = se->song();
        if (song.kit == cur->path) return;
        song.kit = cur->path;
        host_.log(false, (song.name + ": kit " + cur->path).c_str());
    }
    se->commit();
}

bool SongSounds::instrumentHome(std::string& songDir)
{
    ISongEngine* se = songs();
    if (!se || se->openState() != SongOpState::Ready) return false;
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    const AudioSong& song = se->song();
    if (!song.hasPitchTrack()) return false;
    songDir = logical(song.dir);
    return true;
}

void SongSounds::instrumentIs(const std::string& path)
{
    ISongEngine* se = songs();
    if (!se) return;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        AudioSong& song = se->song();
        /* One inside the song's folder is kept relative to it: a song that is
         * renamed takes its instrument along. */
        const std::string home = logical(song.dir) + "/";
        const std::string kept = path.compare(0, home.size(), home) == 0 ? path.substr(home.size()) : path;
        if (song.instrument == kept) return;
        song.instrument = kept;
        host_.log(false, (song.name + ": instrument " + kept).c_str());
    }
    se->commit();
}

void SongSounds::keepInstrument(const KitInfo& spec, const std::function<bool(const KitInfo&)>& save)
{
    std::string home;
    if (!instrumentHome(home)) return;
    if (!spec.path.empty()) {
        instrumentIs(spec.path);
        return;
    }
    KitInfo copy = spec;
    copy.path = home + "/pitch/" + IInstrumentHost::kFileName;
    if (!save || !save(copy)) return;
    instrumentIs(copy.path);
}

} // namespace crosspad

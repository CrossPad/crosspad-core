// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/song/SongSounds.hpp"

#include "crosspad/instrument/IInstrumentHost.hpp"
#include "crosspad/instrument/InstrumentPlayState.hpp"
#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/kit/KitSaved.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sampler/ISecondKit.hpp"
#include "crosspad/song/ISongEngine.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>

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
    /* The engine opens the last song the first time it is asked for it; asked
     * here, at start, so that a kit picked or a take played is the song's from
     * the start instead of being replaced when something else asks later. */
    if (ISongEngine* se = songs()) (void)se->song();
    pending_.store(true);
}

void SongSounds::songOpened()
{
    openGen_.fetch_add(1);
    pending_.store(true, std::memory_order_release);
}

/// Each kit slot's track's kit in that slot's engine, or none.
void SongSounds::keepSecondKit()
{
    ISongEngine* se = songs();
    if (!se || se->openState() != SongOpState::Ready) return;
    ISecondKit* engines[AudioSong::kKitSlots] = {getPlatformServices().secondKit, getPlatformServices().thirdKit};
    std::string want[AudioSong::kKitSlots];
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const AudioSong& song = se->song();
        for (int s = 0; s < AudioSong::kKitSlots; ++s) {
            const int t = song.seqTrackOnRoute(AudioSong::slotRoute(s));
            if (t >= 0) want[s] = song.tracks[t].kit;
        }
    }
    for (int s = 0; s < AudioSong::kKitSlots; ++s) {
        ISecondKit* k = engines[s];
        if (!k) continue;
        k->hold(want[s]);
        if (k->state() == ISecondKit::State::Failed && want[s] != secondKitSaid_[s]) {
            secondKitSaid_[s] = want[s];
            host_.log(true, ("the second kit would not load: " + want[s]).c_str());
            char msg[kWarnChars];
            std::snprintf(msg, sizeof(msg), "The second kit would not load: %s", want[s].c_str());
            host_.warn(msg);
        }
    }
}

/// The saves as of now are the ones the open song's freezes were made after:
/// a song just opened takes its baseline here, not at the first look at a kit,
/// so a save between the two is heard.
void SongSounds::seedSavesHeard()
{
    savesHeard_.clear();
    ISongEngine* se = songs();
    if (!se || se->openState() != SongOpState::Ready) return;
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    const AudioSong& song = se->song();
    for (const auto& t : song.tracks) {
        if (t.kind != TrackKind::Seq || t.source == SeqSource::Pitch) continue;
        const std::string& kit = t.kit.empty() ? song.kit : t.kit;
        if (!kit.empty()) savesHeard_[kit] = kit_saved_generation(kit);
    }
}

/// A kit saved under its own name sounds other than the path in the freeze's
/// key says: the freeze of every seq track that plays it (its own kit, or the
/// song's for the pads' track) is of the old sound, as a Pitch track's is of
/// an instrument edited in place (keepInstrument()).
void SongSounds::freezesHearKitSaves()
{
    ISongEngine* se = songs();
    if (!se || se->openState() != SongOpState::Ready) return;
    if (seededGen_ != openGen_.load()) {
        seededGen_ = openGen_.load();
        seedSavesHeard();
    }
    bool changed = false;
    std::set<std::string> saved;   // the kits this pass found saved: each of their tracks goes stale
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        AudioSong& song = se->song();
        for (auto& t : song.tracks) {
            if (t.kind != TrackKind::Seq || t.source == SeqSource::Pitch) continue;
            const std::string& kit = t.kit.empty() ? song.kit : t.kit;
            if (kit.empty()) continue;
            const uint32_t now = kit_saved_generation(kit);
            const auto heard = savesHeard_.emplace(kit, now).first;
            if (heard->second != now) {
                heard->second = now;
                saved.insert(kit);
            }
            if (!saved.count(kit)) continue;
            t.freezeKey = 0;
            ++t.freezeSalt;
            changed = true;
        }
    }
    if (changed) se->commit();
}

void SongSounds::step()
{
    stepMs_ += kStepMs;
    keepSecondKit();
    freezesHearKitSaves();
    if (!pending_.load(std::memory_order_acquire)) {
        keepPitchPads();
        /* A kit given to the song since it settled -- a kit track's kit set in
         * Arrange -- is brought in as an opening song's is, from the next step. */
        ISongEngine* se = songs();
        if (!se || se->openState() != SongOpState::Ready) return;
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        if (se->song().kit != lastKit_) pending_.store(true, std::memory_order_release);
        return;
    }
    ISongEngine* se = songs();
    IKitManager* kits = getKitManager();
    if (!se || !kits) { pending_.store(false); return; }
    if (se->openState() != SongOpState::Ready) return;
    if (!host_.kitsSettled()) return;
    IInstrumentHost* host = getPlatformServices().instrumentHost;
    if (host && host->state() == IInstrumentHost::State::Busy) return;

    std::string kit, inst;
    bool pitch = false, untouched = false;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const AudioSong& song = se->song();
        kit = song.kit;
        pitch = song.hasPitchTrack();
        untouched = song.usedTracks() == 0 && song.sets.empty() && song.instrument.empty();
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

    lastKit_ = kit;
    KitInfo* cur = kits->getCurrentKit();
    const std::string have = cur ? cur->path : std::string();
    if (kit != have && kit.empty() && cur && untouched) {
        /* A song with nothing in it yet -- a new one -- is started with what the
         * player was playing: it takes the loaded kit instead of giving it back. */
        kitLoaded();
    } else if (kit != have) {
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
        const IInstrumentHost::State st = host->state();
        const bool loaded = st == IInstrumentHost::State::Ready && host->instrument().path == inst;
        if (!loaded) {
            if (inst != instWanted_) {
                instWanted_ = inst;
                instRetry_.reset();
                instAsked_ = instSaid_ = false;
            }
            /* One that will not load -- no memory beside the kits -- is asked for
             * again later, not every step: each try reads it off the card. */
            if (st == IInstrumentHost::State::Failed && instAsked_) {
                instAsked_ = false;
                instRetry_.failed(stepMs_);
                if (!instSaid_) {
                    instSaid_ = true;
                    char msg[kWarnChars];
                    std::snprintf(msg, sizeof(msg), "Pitch sound did not load: %s", host->message().c_str());
                    host_.warn(msg);
                }
            }
            if (!instRetry_.due(stepMs_)) return;
            host_.log(false, ("the song's instrument: " + inst).c_str());
            instAsked_ = host->requestOpen(inst);
            return;
        }
        instRetry_.reset();
        instAsked_ = false;
        /* What Arrange keeps loaded for the Pitch track, and what the
         * Sequencer and the Recorder borrow: this song's. */
        InstrumentPlayState& ps = instrumentPlayState();
        if (ps.instrumentPath != inst) {
            ps.instrumentPath = inst;
            ps.instrumentName = host->instrument().name;
            instrumentPlayStatePublished();
        }
    }
    /* The pads as the song plays them: its map onto the shared state, which
     * the pads, the song's Pitch track and Pitch Play all read. */
    AudioSong::PitchPads pads;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        if (se->song().hasPitchTrack()) pads = se->song().pitchPads;
    }
    InstrumentPlayState& ps = instrumentPlayState();
    if (pads.set) {
        PlaySettings& p = ps.play;
        if (p.rootNote != pads.root || p.octave != pads.octave || p.scaleIndex != pads.scale ||
            ps.transpose != pads.transpose || ps.chordsOn != pads.chords || p.padSilent != pads.silent ||
            std::memcmp(p.padMidiMap, pads.note, sizeof pads.note) != 0 ||
            std::memcmp(p.padChordNotes, pads.chord, sizeof pads.chord) != 0 ||
            std::memcmp(p.padChordCount, pads.count, sizeof pads.count) != 0) {
            p.rootNote = pads.root;
            p.octave = pads.octave;
            p.scaleIndex = pads.scale;
            ps.transpose = pads.transpose;
            ps.chordsOn = pads.chords;
            p.padSilent = pads.silent;
            std::memcpy(p.padMidiMap, pads.note, sizeof pads.note);
            std::memcpy(p.padChordNotes, pads.chord, sizeof pads.chord);
            std::memcpy(p.padChordCount, pads.count, sizeof pads.count);
            instrumentPlayStatePublished();
        }
    }
    padsGen_ = ps.generation;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        settledDir_ = se->song().dir;
    }
    settledGen_.store(openGen_.load());
    pending_.store(false);
}

bool SongSounds::settled() const
{
    if (pending() || !host_.kitsSettled()) return false;
    for (const ISecondKit* k : {getPlatformServices().secondKit, getPlatformServices().thirdKit})
        if (k && k->state() == ISecondKit::State::Loading) return false;
    return true;
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
        /* A kit given to the song while this one loaded is the newer wish:
         * step() waits out a load before it reads the song, so that kit is
         * still to come. Written over, it was lost and the board kept the kit
         * that happened to be loading. */
        if (song.kit != lastKit_) return;
        lastKit_ = cur->path;
        if (song.kit == cur->path) return;
        song.kit = cur->path;
        host_.log(false, (song.name + ": kit " + cur->path).c_str());
    }
    se->commit();
}

/// UI thread, the song settled: a pad map Pitch Play published since is the
/// one the song's Pitch track plays from now on -- kept in the song, whose
/// freeze key it is part of.
void SongSounds::keepPitchPads()
{
    ISongEngine* se = songs();
    const InstrumentPlayState& ps = instrumentPlayState();
    if (!se || se->openState() != SongOpState::Ready || ps.generation == padsGen_) return;
    padsGen_ = ps.generation;
    if (ps.instrumentPath.empty()) return;          // nothing published yet this boot
    AudioSong::PitchPads pads;
    pads.set = true;
    pads.root = ps.play.rootNote;
    pads.octave = ps.play.octave;
    pads.scale = ps.play.scaleIndex;
    pads.transpose = ps.transpose;
    pads.chords = ps.chordsOn;
    pads.silent = ps.play.padSilent;
    std::memcpy(pads.note, ps.play.padMidiMap, sizeof pads.note);
    std::memcpy(pads.chord, ps.play.padChordNotes, sizeof pads.chord);
    std::memcpy(pads.count, ps.play.padChordCount, sizeof pads.count);
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        AudioSong& song = se->song();
        /* Only into the song it was settled for: a song swapped in under an
         * open (a delete opening the next) has not had its own map put on yet. */
        if (song.dir != settledDir_ || settledGen_.load() != openGen_.load()) return;
        if (!song.hasPitchTrack() || song.pitchPads == pads) return;
        song.pitchPads = pads;
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

void SongSounds::keepInstrument(const KitInfo& spec, const std::function<bool(const KitInfo&)>& save,
                                bool changed)
{
    std::string home;
    if (!instrumentHome(home)) return;
    /* Opened before this song's sounds were first settled: not the player's
     * pick but someone still on the last song's instrument, or this song's
     * own being loaded -- which the song already names. */
    if (!changed && settledGen_.load() != openGen_.load()) return;
    if (changed) {
        if (ISongEngine* se = songs()) {
            {
                std::lock_guard<std::recursive_mutex> l(se->songMutex());
                for (auto& t : se->song().tracks)
                    if (t.kind == TrackKind::Seq && t.source == SeqSource::Pitch) {
                        t.freezeKey = 0;
                        ++t.freezeSalt;     // the renders on the card are of the old sound
                    }
            }
            se->commit();
        }
    }
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

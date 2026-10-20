// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/song/SongScenes.hpp"

#include "crosspad/kit/KitPathUtils.hpp"
#include "crosspad/platform/IFileSystem.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sequencer/PatternSequencer.hpp"
#include "crosspad/sequencer/SequenceStore.hpp"
#include "crosspad/song/ISongEngine.hpp"

#include <cstdio>

namespace crosspad {

namespace {

std::string folderOf(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

} // namespace

void SongScenes::kitChanged(const std::string& kitJsonPath)
{
    std::lock_guard<std::mutex> lock(pathMutex_);
    pendingKit_ = kitJsonPath;
}

std::string SongScenes::currentFile() const
{
    std::lock_guard<std::mutex> lock(pathMutex_);
    return shownFile_;
}

void SongScenes::setCurrent(const std::string& path)
{
    current_ = path;
    std::lock_guard<std::mutex> lock(pathMutex_);
    shownFile_ = path;
}

/// The open song's scenes file; "" while no song is open.
std::string SongScenes::wantedFile(const std::string& cardRoot, bool& hasSeqTrack)
{
    hasSeqTrack = false;
    ISongEngine* se = getPlatformServices().songEngine;
    if (!se) return cardRoot + kLegacyFile;
    if (se->openState() != SongOpState::Ready) return {};
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    const AudioSong& song = se->song();
    if (song.dir.empty()) return {};
    hasSeqTrack = song.seqTracks() > 0;
    return song.dir + "/" + SequenceStore::fileName();
}

bool SongScenes::save(const char* why)
{
    if (current_.empty()) return false;
    const bool ok = SequenceStore::save(seq_, current_);
    if (ok) {
        seq_.clearDirty();
        log(false, std::string("saved (") + why + "): " + current_);
    } else {
        /* Left dirty on purpose: the next sweep tries again, which is what
         * makes a card reinserted after a write failure pick the take up. */
        log(true, std::string("save failed (") + why + "): " + current_);
    }
    return ok;
}

/// A song without scenes of its own: a take played before any song was open
/// stays and is its; from before they were the song's, it gets the old shared
/// ones; a card older still, its kit's; else none.
void SongScenes::adopt(const std::string& cardRoot, const std::string& path, bool hasSeqTrack)
{
    const std::string legacy = cardRoot + kLegacyFile;
    const std::string mark = cardRoot + kAdoptedMark;
    const bool first = !getFileSystem().exists(mark.c_str());
    if (current_.empty() && seq_.dirty() && seq_.eventCount() > 0) {
        seq_.markDirty();
        setCurrent(path);
        log(false, path + ": keeps the " + std::to_string(seq_.eventCount()) + " events played before it opened");
    } else if ((first || hasSeqTrack) && SequenceStore::load(seq_, legacy)) {
        seq_.markDirty();
        setCurrent(path);
        log(false, path + ": adopted " + std::to_string(seq_.eventCount()) + " events from " + legacy);
    } else {
        std::string kit;
        {
            std::lock_guard<std::mutex> lock(pathMutex_);
            kit = pendingKit_;
        }
        if (first && !getFileSystem().exists(legacy.c_str()) && !kit.empty() &&
            SequenceStore::load(seq_, kitAssetPath(kit, SequenceStore::fileName()))) {
            seq_.markDirty();
            log(false, path + ": adopted " + std::to_string(seq_.eventCount()) + " events from the kit");
        } else {
            seq_.resetScenes();
            seq_.clearDirty();
            log(false, path + ": no scenes yet");
        }
        setCurrent(path);
    }
    if (first) {
        if (FILE* f = std::fopen(mark.c_str(), "w")) std::fclose(f);
    }
}

/// The open song changed (or its scenes were never read): the last one's
/// changes land in its folder -- unless that went with a delete or a rename --
/// and this one's scenes come in.
void SongScenes::switchTo(const std::string& cardRoot, const std::string& path, bool hasSeqTrack)
{
    if (!current_.empty() && seq_.dirty() && getFileSystem().exists(folderOf(current_).c_str())) {
        save("song change");
    }
    switch (SequenceStore::open(seq_, path)) {
    case SequenceLoad::Loaded:
        setCurrent(path);
        log(false, "loaded " + std::to_string(seq_.eventCount()) + " events from " + path);
        return;
    case SequenceLoad::Unreadable:
        /* This song's grooves are in that file. Not reading it is no reason to
         * write over it, nor to leave the last song's scenes up to be edited
         * and never saved: nothing is shown and nothing saved until it reads,
         * and the sweep asks again -- on the board at boot this is usually the
         * FAT handles, taken by the kit load for a moment. */
        if (warned_ != path) log(true, path + " is there but unreadable; not saving until it reads");
        warned_ = path;
        seq_.resetScenes();
        setCurrent("");
        return;
    case SequenceLoad::Absent:
        adopt(cardRoot, path, hasSeqTrack);
        return;
    }
}

void SongScenes::sweep(const std::string& cardRoot)
{
    bool hasSeqTrack = false;
    const std::string want = wantedFile(cardRoot, hasSeqTrack);
    /* Another song's scenes come in only while nothing plays them: a swap
     * under a running pattern would cut it mid-bar, and one under a take
     * would split it between two songs. */
    if (!want.empty() && want != current_) {
        if (seq_.state() == SequencerState::Idle) switchTo(cardRoot, want, hasSeqTrack);
        return;
    }
    if (current_.empty()) return;

    const bool forced = saveNow_.exchange(false, std::memory_order_acquire);
    if (!forced && !seq_.dirty()) return;
    /* Never mid-take: the pattern is still growing, and the file would be a
     * snapshot of half a performance. Closing the take marks it dirty again,
     * so nothing is lost by waiting. */
    if (seq_.isRecording()) { seq_.markDirty(); return; }
    /* Nor mid-bounce: the take needs every write the card can do, and it
     * dirties the scenes itself (selectScene). It lands after. */
    ISongEngine* song = getPlatformServices().songEngine;
    if (song && song->bounceStatus().state == BounceState::Running) { seq_.markDirty(); return; }

    save(forced ? "requested" : "dirty");
}

} // namespace crosspad

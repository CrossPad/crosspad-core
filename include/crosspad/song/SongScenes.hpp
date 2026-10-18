// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <mutex>
#include <string>

namespace crosspad {

class PatternSequencer;

/**
 * @brief The sequencer's scenes as the open song's: read from its folder and
 *        written back there whenever they change.
 *
 * The scenes are the song's: <song>/sequences.json, beside song.json, so
 * opening another song opens its grooves and a new song starts with none.
 * Before that one file held every song's scenes (kLegacyFile), and a seq track
 * in one song played whatever another had left in them. A song from before
 * that has no file of its own is given a copy of that old one: the song open
 * the first time this runs (whatever the player had in the Sequencer is
 * theirs; kAdoptedMark says that happened), and any song with a seq track
 * (what it played is in there). Every other song starts empty.
 *
 * The scenes are not a kit's either. Picking another kit keeps the groove and
 * plays it on the new kit's pads, the way a drum machine swaps its sounds.
 * Scenes used to live beside each kit; a card still on that adopts its
 * booting kit's file once, when it has no other.
 *
 * The platform runs sweep() from a task of its own every half second or so,
 * and only while a card is in: a card write belongs neither on the UI thread
 * nor on the sequencer's poll. Reads the song engine from PlatformServices; a
 * platform without one keeps the one legacy file.
 */
class SongScenes {
public:
    using LogFn = void (*)(bool warning, const char* msg);

    /// Where every song's scenes were kept until they moved into the songs,
    /// under the card's root. Read only to hand a song from before its scenes.
    static constexpr const char* kLegacyFile = "/crosspad/sequences.json";
    /// Left beside it once the song open the first time has adopted it.
    static constexpr const char* kAdoptedMark = "/crosspad/sequences.adopted";

    SongScenes(PatternSequencer& seq, LogFn log) : seq_(seq), log_(log) {}

    /// A kit became current (its kit.json path). Only a card that never had
    /// shared scenes reads anything of it. Cheap, any thread.
    void kitChanged(const std::string& kitJsonPath);

    /// Write on the next sweep even if nothing is marked dirty. Any thread.
    void saveSoon() { saveNow_.store(true, std::memory_order_release); }

    /// The file the scenes are read from and written to -- the open song's --
    /// "" until one was read. Any thread.
    std::string currentFile() const;

    /// One pass of the platform's task, with the card's root at @p cardRoot
    /// ("/sdcard" on the board): another song's scenes come in, or changed
    /// ones go out.
    void sweep(const std::string& cardRoot);

private:
    std::string wantedFile(const std::string& cardRoot, bool& hasSeqTrack);
    void setCurrent(const std::string& path);
    bool save(const char* why);
    void adopt(const std::string& cardRoot, const std::string& path, bool hasSeqTrack);
    void switchTo(const std::string& cardRoot, const std::string& path, bool hasSeqTrack);
    void log(bool warning, const std::string& msg) { if (log_) log_(warning, msg.c_str()); }

    PatternSequencer& seq_;
    LogFn log_;
    mutable std::mutex pathMutex_;
    std::string pendingKit_;          // under pathMutex_
    std::string shownFile_;           // current_ for other threads, under pathMutex_
    std::string current_;             // the sweep's; empty until a file was read
    std::string warned_;              // the sweep's: an unreadable file said once
    std::atomic<bool> saveNow_{false};
};

} // namespace crosspad

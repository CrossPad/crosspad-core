// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace crosspad {

/// One block on an audio track: a WAV from the project's clips/ folder,
/// placed on a bar. It plays once, from its first sample, for as long as the
/// file is -- @c bars is the length it was made to (a bounce knows it), and is
/// what the timeline draws and what "after the last block" is counted from.
struct SongClip {
    std::string file;        ///< name inside <project>/clips/
    uint32_t    bar  = 0;    ///< 0-based start bar
    uint16_t    bars = 1;    ///< length on the grid
};

/// What AudioSong::load() found. Unreadable (corrupt, or saved by newer
/// firmware) is kept apart from New so nobody saves an empty song over it.
enum class SongLoad : uint8_t { New, Loaded, Unreadable };

struct SongTrack {
    std::vector<SongClip> clips;   ///< kept sorted by bar
    float volume = 1.0f;
    bool  muted  = false;
};

/// An audio song: four tracks of clips on a bar grid, at one tempo.
///
/// A project is a folder -- <root>/<name>/song.json and <root>/<name>/clips/
/// -- so clips bounced from different kits can sit in one song. The grid is
/// the song's bpm and metre; a clip made at another tempo plays at the speed it
/// was recorded (there is no time-stretch), which is why a bounce records the
/// tempo it was made at and the song adopts the first one it is given.
///
/// Plain data plus load/save: portable, no locking. Whoever plays it takes a
/// copy (see ISongEngine::commit), so editing never races playback.
class AudioSong {
public:
    static constexpr int kTracks = 4;
    static constexpr int kVersion = 1;
    static constexpr float kDefaultBpm = 120.0f;       ///< the grid before a clip sets one
    static constexpr uint8_t kDefaultBeatsPerBar = 4;
    static const char* fileName() { return "song.json"; }
    static const char* clipsDir() { return "clips"; }

    std::string dir;              ///< project folder, no trailing slash
    std::string name;
    float       bpm  = 0.0f;      ///< 0 until the first clip sets it
    uint8_t     beatsPerBar = kDefaultBeatsPerBar;
    bool        loop = true;
    uint32_t    length = 0;       ///< bars the song plays (and loops) over; 0 = to its last block
    bool        syncSeq = false;  ///< playing the song also plays the sequencer, locked to it
    SongTrack   tracks[kTracks];

    SongLoad load(const std::string& projectDir);
    /// Written beside song.json and swapped in, the old file kept as .bak
    /// until the new one is in place; load() falls back to either, so a cut
    /// at any point costs at most the edit being saved.
    bool save() const;

    std::string clipPath(const std::string& file) const { return dir + "/" + clipsDir() + "/" + file; }

    /// The bar after a track's last block, where "append" puts the next one.
    uint32_t trackEnd(int track) const;
    /// The bar after the last block on any track.
    uint32_t songEnd() const;
    /// Where playback ends or loops: the set length, or the last block's end.
    uint32_t playEnd() const { return length ? length : songEnd(); }
    /// Add a block, keeping the track sorted. Returns the index of the block
    /// added, also when an identical one sits on the same bar.
    int addClip(int track, const SongClip& clip);
    void sortTrack(int track);

    /// Samples in one bar at the song's tempo, at @p sampleRate.
    uint32_t samplesPerBar(uint32_t sampleRate) const;
};

} // namespace crosspad

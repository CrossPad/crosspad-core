// SPDX-License-Identifier: MIT
#include "crosspad/song/AudioSong.hpp"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace crosspad {

bool AudioSong::load(const std::string& projectDir)
{
    dir = projectDir;
    const size_t slash = projectDir.find_last_of('/');
    name = slash == std::string::npos ? projectDir : projectDir.substr(slash + 1);
    bpm = 0.0f;
    loop = true;
    length = 0;
    syncSeq = false;
    for (auto& t : tracks) t = SongTrack{};

    std::ifstream f(dir + "/" + fileName());
    if (!f.is_open()) return false;          // a new project: empty is right
    JsonDocument doc;
    if (deserializeJson(doc, f)) return false;
    if ((doc["version"] | 0) > kVersion) return false;

    bpm  = doc["bpm"]  | 0.0f;
    loop = doc["loop"] | true;
    length = doc["length"] | 0u;
    syncSeq = doc["sync"] | false;
    int t = 0;
    for (JsonObjectConst tr : doc["tracks"].as<JsonArrayConst>()) {
        if (t >= kTracks) break;
        tracks[t].volume = tr["volume"] | 1.0f;
        tracks[t].muted  = tr["muted"]  | false;
        for (JsonObjectConst c : tr["clips"].as<JsonArrayConst>()) {
            SongClip clip;
            clip.file = c["file"] | "";
            clip.bar  = c["bar"]  | 0u;
            clip.bars = c["bars"] | 1u;
            if (!clip.file.empty() && clip.bars) tracks[t].clips.push_back(clip);
        }
        sortTrack(t);
        ++t;
    }
    return true;
}

bool AudioSong::save() const
{
    JsonDocument doc;
    doc["version"] = kVersion;
    doc["bpm"]  = bpm;
    doc["loop"] = loop;
    doc["length"] = length;
    doc["sync"] = syncSeq;
    JsonArray trs = doc["tracks"].to<JsonArray>();
    for (const auto& t : tracks) {
        JsonObject o = trs.add<JsonObject>();
        o["volume"] = t.volume;
        o["muted"]  = t.muted;
        JsonArray cs = o["clips"].to<JsonArray>();
        for (const auto& c : t.clips) {
            JsonObject co = cs.add<JsonObject>();
            co["file"] = c.file;
            co["bar"]  = c.bar;
            co["bars"] = c.bars;
        }
    }

    /* Beside the real file and renamed over it: a card pulled mid-write
     * costs the edit, never the song. */
    const std::string path = dir + "/" + fileName();
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f.is_open()) return false;
        serializeJson(doc, f);
        if (!f) { f.close(); std::remove(tmp.c_str()); return false; }
    }
    std::remove(path.c_str());          // rename() will not replace on FatFs
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

uint32_t AudioSong::trackEnd(int track) const
{
    if (track < 0 || track >= kTracks) return 0;
    uint32_t end = 0;
    for (const auto& c : tracks[track].clips) end = std::max(end, c.bar + c.bars);
    return end;
}

uint32_t AudioSong::songEnd() const
{
    uint32_t end = 0;
    for (int t = 0; t < kTracks; ++t) end = std::max(end, trackEnd(t));
    return end;
}

int AudioSong::addClip(int track, const SongClip& clip)
{
    if (track < 0 || track >= kTracks) return -1;
    auto& cs = tracks[track].clips;
    cs.push_back(clip);
    sortTrack(track);
    for (size_t i = 0; i < cs.size(); ++i) {
        if (cs[i].bar == clip.bar && cs[i].file == clip.file) return (int)i;
    }
    return (int)cs.size() - 1;
}

void AudioSong::sortTrack(int track)
{
    if (track < 0 || track >= kTracks) return;
    auto& cs = tracks[track].clips;
    std::stable_sort(cs.begin(), cs.end(),
                     [](const SongClip& a, const SongClip& b) { return a.bar < b.bar; });
}

uint32_t AudioSong::samplesPerBar(uint32_t sampleRate) const
{
    /* The beat truncated first, the way MetronomeNode counts it: a bounce is
     * exactly bars * 4 of those beats, and a bar computed any other way drifts
     * a few samples per bar against the clips laid on it. */
    const float b = bpm > 0.0f ? bpm : 120.0f;
    return (uint32_t)((float)sampleRate * 60.0f / b) * 4u;
}

} // namespace crosspad

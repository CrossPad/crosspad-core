// SPDX-License-Identifier: MIT
#include "crosspad/song/AudioSong.hpp"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace crosspad {

namespace {

enum class Parse : uint8_t { Missing, Bad, Newer, Ok };

Parse parseFile(const std::string& path, JsonDocument& doc)
{
    std::ifstream f(path);
    if (!f.is_open()) return Parse::Missing;
    if (deserializeJson(doc, f)) return Parse::Bad;
    if ((doc["version"] | 0) > AudioSong::kVersion) return Parse::Newer;
    return Parse::Ok;
}

} // namespace

static_assert(ClipSet::kCols == AudioSong::kTracks, "a set's row is one cell per track");

namespace {
const std::string kNoCell;
}

const std::string& ClipSet::at(uint32_t row, int col) const
{
    return inside(row, col) ? cells[row * kCols + col] : kNoCell;
}

void ClipSet::put(uint32_t row, int col, const std::string& file)
{
    if (col < 0 || col >= kCols || row >= kMaxRows) return;
    if (row >= rows()) setRows(row + 1);
    cells[row * kCols + col] = file;
}

void ClipSet::setRows(uint32_t n)
{
    cells.resize((size_t)std::min(n, kMaxRows) * kCols);
}

bool ClipSet::blank() const
{
    for (const auto& c : cells) if (!c.empty()) return false;
    return true;
}

ClipSet clipSetFromFiles(const std::vector<std::string>& files, const std::string& name)
{
    ClipSet set;
    set.name = name;
    set.setRows((uint32_t)((files.size() + ClipSet::kCols - 1) / ClipSet::kCols));
    for (size_t i = 0; i < files.size() && i < set.cells.size(); ++i) set.cells[i] = files[i];
    return set;
}

ClipSet* AudioSong::currentClipSet()
{
    return currentSet < sets.size() ? &sets[currentSet] : nullptr;
}

const ClipSet* AudioSong::currentClipSet() const
{
    return currentSet < sets.size() ? &sets[currentSet] : nullptr;
}

int AudioSong::addClipSet(const ClipSet& set)
{
    if ((int)sets.size() >= kMaxSets) return -1;
    sets.push_back(set);
    currentSet = (uint8_t)(sets.size() - 1);
    return (int)currentSet;
}

void AudioSong::removeClipSet(int index)
{
    if (index < 0 || index >= (int)sets.size()) return;
    sets.erase(sets.begin() + index);
    if (sets.empty()) currentSet = 0;
    else if (currentSet >= sets.size()) currentSet = (uint8_t)(sets.size() - 1);
}

SongLoad AudioSong::load(const std::string& projectDir)
{
    dir = projectDir;
    const size_t slash = projectDir.find_last_of('/');
    name = slash == std::string::npos ? projectDir : projectDir.substr(slash + 1);
    bpm = 0.0f;
    beatsPerBar = kDefaultBeatsPerBar;
    loop = true;
    length = 0;
    syncSeq = false;
    loopStart = loopEnd = 0;
    for (auto& t : tracks) t = SongTrack{};
    sets.clear();
    currentSet = 0;

    /* song.json, else what save() leaves when it is cut short: the new file
     * complete beside it (.tmp), or only the old one moved aside (.bak). */
    const std::string path = dir + "/" + fileName();
    JsonDocument doc;
    bool any = false;
    bool found = false;
    for (const char* suffix : {"", ".tmp", ".bak"}) {
        doc.clear();
        const Parse p = parseFile(path + suffix, doc);
        if (p == Parse::Newer) return SongLoad::Unreadable;
        any = any || p != Parse::Missing;
        if (p == Parse::Ok) { found = true; break; }
    }
    if (!found) return any ? SongLoad::Unreadable : SongLoad::New;

    bpm  = doc["bpm"]  | 0.0f;
    beatsPerBar = doc["bpb"] | kDefaultBeatsPerBar;
    if (!beatsPerBar) beatsPerBar = kDefaultBeatsPerBar;
    loop = doc["loop"] | true;
    length = doc["length"] | 0u;
    syncSeq = doc["sync"] | false;
    loopStart = doc["loop_start"] | 0u;
    loopEnd = doc["loop_end"] | 0u;
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
    /* A row shorter or longer than the grid is padded or cut rather than
     * refused: a set edited by hand should not cost somebody their song. */
    for (JsonObjectConst so : doc["sets"].as<JsonArrayConst>()) {
        if ((int)sets.size() >= kMaxSets) break;
        ClipSet set;
        set.name = so["name"] | "";
        for (JsonArrayConst row : so["rows"].as<JsonArrayConst>()) {
            if (set.rows() >= ClipSet::kMaxRows) break;
            const uint32_t r = set.rows();
            set.setRows(r + 1);
            int c = 0;
            for (JsonVariantConst cell : row) {
                if (c >= ClipSet::kCols) break;
                set.put(r, c++, cell.as<const char*>() ? cell.as<const char*>() : "");
            }
        }
        sets.push_back(set);
    }
    currentSet = doc["set"] | 0u;
    if (currentSet >= sets.size()) currentSet = 0;
    return SongLoad::Loaded;
}

bool AudioSong::save() const
{
    JsonDocument doc;
    doc["version"] = kVersion;
    doc["bpm"]  = bpm;
    doc["bpb"]  = beatsPerBar;
    doc["loop"] = loop;
    doc["length"] = length;
    doc["sync"] = syncSeq;
    doc["loop_start"] = loopStart;
    doc["loop_end"] = loopEnd;
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

    if (!sets.empty()) {
        JsonArray ss = doc["sets"].to<JsonArray>();
        for (const auto& set : sets) {
            JsonObject so = ss.add<JsonObject>();
            so["name"] = set.name;
            JsonArray rows = so["rows"].to<JsonArray>();
            for (uint32_t r = 0; r < set.rows(); ++r) {
                JsonArray row = rows.add<JsonArray>();
                for (int c = 0; c < ClipSet::kCols; ++c) row.add(set.at(r, c));
            }
        }
        doc["set"] = currentSet;
    }

    const std::string path = dir + "/" + fileName();
    const std::string tmp = path + ".tmp";
    const std::string bak = path + ".bak";
    {
        std::ofstream f(tmp);
        if (!f.is_open()) return false;
        serializeJson(doc, f);
        f.close();                      // a full card shows up on the flush, not the write
        if (!f) { std::remove(tmp.c_str()); return false; }
    }
    /* rename() will not replace a file on FatFs, so the old one steps aside
     * first -- as .bak, not removed, so there is never a moment with neither. */
    std::remove(bak.c_str());
    const bool hadOld = std::rename(path.c_str(), bak.c_str()) == 0;
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        if (hadOld) std::rename(bak.c_str(), path.c_str());
        std::remove(tmp.c_str());
        return false;
    }
    std::remove(bak.c_str());
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
    /* The sort is stable and the block went in last, so it is the last one
     * on its bar. */
    for (size_t i = cs.size(); i-- > 0;) {
        if (cs[i].bar == clip.bar) return (int)i;
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
     * exactly bars * beatsPerBar of those beats, and a bar computed any other
     * way drifts a few samples per bar against the clips laid on it. */
    const float b = bpm > 0.0f ? bpm : kDefaultBpm;
    return (uint32_t)((float)sampleRate * 60.0f / b) * beatsPerBar;
}

} // namespace crosspad

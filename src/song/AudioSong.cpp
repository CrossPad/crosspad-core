// SPDX-License-Identifier: MIT
#include "crosspad/song/AudioSong.hpp"
#include "../util/JsonStdio.hpp"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace crosspad {

namespace {

enum class Parse : uint8_t { Missing, Bad, Newer, Ok };

Parse parseFile(const std::string& path, JsonDocument& doc)
{
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return Parse::Missing;
    JsonFileReader in(f);
    const DeserializationError err = deserializeJson(doc, in);
    std::fclose(f);
    if (err) return Parse::Bad;
    if ((doc["version"] | 0) > AudioSong::kVersion) return Parse::Newer;
    return Parse::Ok;
}

/* A seq block's notes in song.json: one string per block, a note per
 * space-separated entry, "tick,pad,vel[,len[,perf]]" -- the trailing fields
 * only where they are not 0, as sequences.json writes its events. One string
 * rather than an array of arrays because the document holds every value as a
 * node of its own: a few thousand notes were a few hundred kB of PSRAM for as
 * long as the song was being read or written. */
void writeNotes(const SeqPattern& p, std::string& out)
{
    out.clear();
    out.reserve(p.notes.size() * 14);
    char buf[48];
    for (const SeqNote& n : p.notes) {
        int len = 0;
        if (n.perf)     len = std::snprintf(buf, sizeof buf, "%u,%u,%u,%u,%u", (unsigned)n.tick, (unsigned)n.pad,
                                            (unsigned)n.vel, (unsigned)n.len, (unsigned)n.perf);
        else if (n.len) len = std::snprintf(buf, sizeof buf, "%u,%u,%u,%u", (unsigned)n.tick, (unsigned)n.pad,
                                            (unsigned)n.vel, (unsigned)n.len);
        else            len = std::snprintf(buf, sizeof buf, "%u,%u,%u", (unsigned)n.tick, (unsigned)n.pad,
                                            (unsigned)n.vel);
        if (!out.empty()) out += ' ';
        out.append(buf, (size_t)len);
    }
}

/// writeNotes() undone. A malformed entry is skipped, not the block.
std::vector<SeqNote> readNotes(const char* s)
{
    std::vector<SeqNote> out;
    while (s && *s) {
        while (*s == ' ') ++s;
        if (!*s) break;
        uint32_t f[5] = {};
        int n = 0;
        char* end = nullptr;
        while (n < 5) {
            f[n++] = (uint32_t)std::strtoul(s, &end, 10);
            if (end == s) { n = 0; break; }
            s = end;
            if (*s != ',') break;
            ++s;
        }
        if (n >= 3 && f[1] < 16 && f[2]) {
            SeqNote note;
            note.tick = f[0];
            note.pad  = (uint8_t)f[1];
            note.vel  = (uint8_t)std::min<uint32_t>(f[2], 127);
            note.len  = n >= 4 ? f[3] : 0;
            note.perf = n >= 5 ? (uint8_t)f[4] : 0;
            out.push_back(note);
        }
        while (*s && *s != ' ') ++s;
    }
    return out;
}

} // namespace

SeqPatternPtr makeSeqPattern(uint16_t bars, std::vector<SeqNote> notes)
{
    auto p = std::make_shared<SeqPattern>();
    p->bars = bars ? bars : 1;
    std::stable_sort(notes.begin(), notes.end(),
                     [](const SeqNote& a, const SeqNote& b) { return a.tick < b.tick; });
    p->notes = std::move(notes);
    return p;
}

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
        tracks[t].solo   = tr["solo"]   | false;
        tracks[t].name   = tr["name"]   | "";
        tracks[t].kind   = (tr["kind"] | 0) == 1 ? TrackKind::Seq : TrackKind::Audio;
        tracks[t].source = (tr["src"]  | 0) == 1 ? SeqSource::Pitch : SeqSource::Kit;
        tracks[t].scene  = (uint8_t)(tr["scene"] | 0u);
        tracks[t].seqBlocks = tracks[t].kind == TrackKind::Seq && (tr["blocks"] | false);
        /* true keeps the pitch (what 1.57 first wrote), "repitch" moves it. */
        const char* stretch = tr["stretch"].is<const char*>() ? tr["stretch"].as<const char*>() : nullptr;
        tracks[t].stretch = tracks[t].kind != TrackKind::Audio            ? StretchMode::Off
                          : stretch && std::strcmp(stretch, "repitch") == 0 ? StretchMode::Repitch
                          : (tr["stretch"] | false)                         ? StretchMode::KeepPitch
                                                                            : StretchMode::Off;
        /* A song saved before tracks had kinds has no "used" key: the clips
         * say which tracks somebody made. Written out from then on, so a
         * track emptied by hand stays a track. */
        tracks[t].inUse  = tr["used"] | false;
        for (JsonObjectConst c : tr["clips"].as<JsonArrayConst>()) {
            SongClip clip;
            clip.file = c["file"] | "";
            clip.bar  = c["bar"]  | 0u;
            clip.bars = c["bars"] | 1u;
            clip.offset = c["offset"] | 0u;
            clip.frames = c["frames"] | 0u;
            clip.from = c["from"] | 0u;
            /* A seq track's blocks have no file: they are notes, their own
             * or the track's scene. */
            const bool seq = tracks[t].kind == TrackKind::Seq;
            JsonObjectConst notes = c["notes"];
            if (seq && !notes.isNull())
                clip.notes = makeSeqPattern((uint16_t)(notes["bars"] | 1u), readNotes(notes["ev"] | ""));
            if ((seq || !clip.file.empty()) && clip.bars) tracks[t].clips.push_back(clip);
        }
        sortTrack(t);
        if (!tr["used"].is<bool>())
            tracks[t].inUse = !tracks[t].clips.empty() || tracks[t].kind == TrackKind::Seq;
        ++t;
    }
    /* Two seq tracks cannot both play: the sequencer has one scene running.
     * A file that claims them (hand-edited, or written by firmware that
     * allowed more) keeps the first and the rest become plain audio tracks --
     * their clips, if any, still play. */
    for (int i = 0, seq = 0; i < kTracks; ++i) {
        if (tracks[i].kind != TrackKind::Seq) continue;
        if (++seq > kMaxSeqTracks) {
            tracks[i].kind = TrackKind::Audio;
            tracks[i].seqBlocks = false;
            dropSeqBlocks(i);
        }
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
    bool notes = false;
    for (const auto& t : tracks)
        for (const auto& c : t.clips) notes = notes || (t.kind == TrackKind::Seq && c.notes);
    JsonDocument doc;
    doc["version"] = notes ? kVersion : 1;
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
        if (t.solo) o["solo"] = true;
        if (!t.name.empty()) o["name"] = t.name;
        o["used"]   = t.inUse;
        if (t.kind == TrackKind::Seq) {
            o["kind"]  = 1;
            o["src"]   = t.source == SeqSource::Pitch ? 1 : 0;
            o["scene"] = t.scene;
            if (t.seqBlocks) o["blocks"] = true;
        }
        if (t.kind == TrackKind::Audio && t.stretch == StretchMode::KeepPitch) o["stretch"] = true;
        if (t.kind == TrackKind::Audio && t.stretch == StretchMode::Repitch)   o["stretch"] = "repitch";
        JsonArray cs = o["clips"].to<JsonArray>();
        std::string ev;
        for (const auto& c : t.clips) {
            JsonObject co = cs.add<JsonObject>();
            if (!c.file.empty()) co["file"] = c.file;
            co["bar"]  = c.bar;
            co["bars"] = c.bars;
            /* Only where set: an uncut block reads as it always did. */
            if (c.offset) co["offset"] = c.offset;
            if (c.frames) co["frames"] = c.frames;
            if (c.from) co["from"] = c.from;
            if (t.kind == TrackKind::Seq && c.notes) {
                JsonObject no = co["notes"].to<JsonObject>();
                no["bars"] = c.notes->bars;
                writeNotes(*c.notes, ev);
                no["ev"] = ev;          // copied in: ev is reused for the next block
            }
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
        FILE* f = std::fopen(tmp.c_str(), "w");
        if (!f) return false;
        JsonFileWriter out{f};
        serializeJson(doc, out);
        if (!closeWritten(f)) { std::remove(tmp.c_str()); return false; }
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

// ── tracks a player makes ──────────────────────────────────────────────────

int AudioSong::usedTracks() const
{
    int n = 0;
    for (const auto& t : tracks) n += t.inUse ? 1 : 0;
    return n;
}

int AudioSong::seqTracks() const
{
    int n = 0;
    for (const auto& t : tracks) n += (t.inUse && t.kind == TrackKind::Seq) ? 1 : 0;
    return n;
}

bool AudioSong::anySolo() const
{
    for (const auto& t : tracks) if (t.solo) return true;
    return false;
}

bool AudioSong::silenced(int t) const
{
    if (t < 0 || t >= kTracks) return true;
    return tracks[t].muted || (anySolo() && !tracks[t].solo);
}

int AudioSong::freeTrack() const
{
    for (int t = 0; t < kTracks; ++t) if (!tracks[t].inUse) return t;
    return -1;
}

bool AudioSong::addAudioTrack(int track)
{
    if (track < 0 || track >= kTracks || tracks[track].inUse) return false;
    tracks[track] = SongTrack{};
    tracks[track].inUse = true;
    return true;
}

bool AudioSong::addSeqTrack(int track, SeqSource source, uint8_t scene)
{
    if (track < 0 || track >= kTracks || tracks[track].inUse) return false;
    if (!seqSlotFree()) return false;
    tracks[track] = SongTrack{};
    tracks[track].inUse  = true;
    tracks[track].kind   = TrackKind::Seq;
    tracks[track].source = source;
    tracks[track].scene  = scene;
    return true;
}

bool AudioSong::bounceSeqTrack(int track, const SongClip* clip)
{
    if (track < 0 || track >= kTracks) return false;
    if (!tracks[track].inUse || tracks[track].kind != TrackKind::Seq) return false;
    /* The notes became one file: the track holds that and nothing else, so
     * what it plays after the bounce is what was heard before it. The source
     * is left as it was -- it says which app wrote this, and a track bounced
     * by mistake is one DEL and one new seq track away from where it was. */
    tracks[track].kind = TrackKind::Audio;
    if (clip) {
        tracks[track].clips.clear();
        if (!clip->file.empty() && clip->bars) tracks[track].clips.push_back(*clip);
    }
    /* The scene's blocks are in the file now; one left behind would be an
     * audio block with no audio. */
    tracks[track].seqBlocks = false;
    dropSeqBlocks(track);
    return true;
}

void AudioSong::dropSeqBlocks(int track)
{
    auto& cs = tracks[track].clips;
    cs.erase(std::remove_if(cs.begin(), cs.end(), [](const SongClip& c) { return c.file.empty(); }),
             cs.end());
}

// ── editing by bars ────────────────────────────────────────────────────────

uint32_t AudioSong::soundingEnd(int track, int index) const
{
    const auto& cs = tracks[track].clips;
    uint32_t end = cs[index].bar + cs[index].bars;
    if (index + 1 < (int)cs.size()) end = std::min(end, cs[index + 1].bar);
    return end;
}

SongClip AudioSong::part(int track, const SongClip& c, uint32_t skip, uint32_t bars,
                         uint32_t spBar) const
{
    SongClip o = c;
    o.bar  = c.bar + skip;
    o.bars = (uint16_t)bars;
    if (tracks[track].kind == TrackKind::Seq) {
        o.from = (uint16_t)(c.from + skip);
        return o;
    }
    const uint32_t head = skip * spBar;
    o.offset = c.offset + head;
    if (c.frames) o.frames = c.frames > head ? c.frames - head : 1;
    /* Stopped short of where the block ended: the length is kept in frames,
     * or a tempo change that re-measures blocks by their files would undo it. */
    if (skip + bars < c.bars) {
        const uint32_t keep = bars * spBar;
        if (!o.frames || o.frames > keep) o.frames = keep;
    }
    return o;
}

int AudioSong::cutClip(int track, uint32_t bar, uint32_t spBar)
{
    if (track < 0 || track >= kTracks) return -1;
    auto& cs = tracks[track].clips;
    for (int i = 0; i < (int)cs.size(); ++i) {
        const SongClip c = cs[i];
        const uint32_t end = soundingEnd(track, i);
        if (bar <= c.bar || bar >= end) continue;
        const uint32_t before = bar - c.bar;
        const SongClip tail = part(track, c, before, c.bar + c.bars - bar, spBar);
        cs[i] = part(track, c, 0, before, spBar);
        return addClip(track, tail);
    }
    return -1;
}

void AudioSong::insertBars(uint32_t at, uint32_t by, uint32_t spBar)
{
    if (!by) return;
    for (int t = 0; t < kTracks; ++t) {
        cutClip(t, at, spBar);
        for (auto& c : tracks[t].clips) if (c.bar >= at) c.bar += by;
    }
    if (length && length > at) length += by;
}

std::vector<RangeClip> AudioSong::copyRange(uint32_t from, uint32_t to, uint32_t spBar) const
{
    std::vector<RangeClip> out;
    for (int t = 0; t < kTracks; ++t) {
        const auto& cs = tracks[t].clips;
        for (int i = 0; i < (int)cs.size(); ++i) {
            const uint32_t a = std::max(cs[i].bar, from);
            const uint32_t b = std::min(soundingEnd(t, i), to);
            if (a >= b) continue;
            out.push_back(RangeClip{t, a - from, part(t, cs[i], a - cs[i].bar, b - a, spBar)});
        }
    }
    return out;
}

void AudioSong::pasteRange(const std::vector<RangeClip>& clips, uint32_t at)
{
    for (const auto& k : clips) {
        if (k.track < 0 || k.track >= kTracks) continue;
        /* A seq block goes only where the seq is, an audio one only onto
         * audio: a track that changed kind since the copy is skipped. */
        const bool seq = tracks[k.track].kind == TrackKind::Seq;
        if (seq != k.clip.file.empty()) continue;
        SongClip c = k.clip;
        c.bar = at + k.rel;
        tracks[k.track].clips.push_back(c);
    }
    for (int t = 0; t < kTracks; ++t) sortTrack(t);
}

bool AudioSong::seqBlocksExplicit(uint32_t bars)
{
    bool made = false;
    for (int t = 0; t < kTracks; ++t) {
        auto& tr = tracks[t];
        if (!tr.inUse || tr.kind != TrackKind::Seq || tr.seqBlocks) continue;
        tr.clips.clear();
        SongClip c;
        c.bars = (uint16_t)std::max<uint32_t>(1, std::min<uint32_t>(bars, 0xFFFF));
        tr.clips.push_back(c);
        tr.seqBlocks = true;
        made = true;
    }
    return made;
}

int AudioSong::adoptSceneNotes(int track, const SeqPatternPtr& scene, uint32_t bars)
{
    if (track < 0 || track >= kTracks || !scene) return 0;
    auto& tr = tracks[track];
    if (!tr.inUse || tr.kind != TrackKind::Seq) return 0;
    auto& cs = tr.clips;
    if (!tr.seqBlocks) {
        cs.clear();
        SongClip c;
        c.bars = (uint16_t)std::max<uint32_t>(1, std::min<uint32_t>(bars, 0xFFFF));
        cs.push_back(c);
        tr.seqBlocks = true;
    }
    int changed = 0;
    /* An empty scene leaves nothing to adopt: a block of it is a block of
     * silence, and one the player never put there. */
    if (scene->notes.empty()) {
        const size_t was = cs.size();
        cs.erase(std::remove_if(cs.begin(), cs.end(), [](const SongClip& c) { return !c.notes; }), cs.end());
        return (int)(was - cs.size());
    }
    for (auto& c : cs) {
        if (c.notes) continue;
        c.notes = scene;
        ++changed;
    }
    return changed;
}

int AudioSong::putSeqTake(int track, uint32_t bar, const SeqPatternPtr& pattern, uint32_t spBar)
{
    if (track < 0 || track >= kTracks || !pattern) return -1;
    auto& tr = tracks[track];
    if (!tr.inUse || tr.kind != TrackKind::Seq) return -1;
    const uint32_t end = bar + pattern->bars;
    cutClip(track, bar, spBar);
    cutClip(track, end, spBar);
    auto& cs = tr.clips;
    cs.erase(std::remove_if(cs.begin(), cs.end(),
                            [&](const SongClip& c) { return c.bar >= bar && c.bar < end; }),
             cs.end());
    tr.seqBlocks = true;
    SongClip c;
    c.bar = bar;
    c.bars = pattern->bars;
    c.notes = pattern;
    return addClip(track, c);
}

bool AudioSong::dubSeqNotes(int track, const std::vector<SeqNote>& notes)
{
    if (track < 0 || track >= kTracks) return false;
    auto& tr = tracks[track];
    if (!tr.inUse || tr.kind != TrackKind::Seq) return false;
    tr.seqBlocks = true;
    const uint32_t tpb = seqTicksPerBar();
    auto& cs = tr.clips;
    std::vector<std::vector<SeqNote>> into(cs.size());
    std::vector<SeqNote> loose;
    for (const SeqNote& n : notes) {
        const uint32_t bar = n.tick / tpb;
        int under = -1;
        for (int i = 0; i < (int)cs.size() && under < 0; ++i)
            if (bar >= cs[i].bar && bar < soundingEnd(track, i)) under = i;
        if (under < 0) { loose.push_back(n); continue; }
        const SongClip& c = cs[under];
        if (!c.notes) continue;       // the scene's notes are not the song's to change
        const uint64_t loop = (uint64_t)c.notes->bars * tpb;
        SeqNote m = n;
        m.tick = (uint32_t)(((uint64_t)n.tick - (uint64_t)c.bar * tpb + (uint64_t)c.from * tpb) % loop);
        into[under].push_back(m);
    }
    for (size_t i = 0; i < into.size(); ++i) {
        if (into[i].empty()) continue;
        std::vector<SeqNote> all = cs[i].notes->notes;
        all.insert(all.end(), into[i].begin(), into[i].end());
        cs[i].notes = makeSeqPattern(cs[i].notes->bars, std::move(all));
    }
    /* What fell where nothing sounded is a block of its own per gap: whole
     * bars from its first note to its last, up to the next block. */
    std::stable_sort(loose.begin(), loose.end(),
                     [](const SeqNote& a, const SeqNote& b) { return a.tick < b.tick; });
    for (size_t i = 0; i < loose.size();) {
        const uint32_t first = loose[i].tick / tpb;
        uint32_t limit = UINT32_MAX;
        for (const auto& c : cs) if (c.bar > first) limit = std::min(limit, c.bar);
        size_t j = i;
        uint32_t last = first;
        while (j < loose.size() && loose[j].tick / tpb < limit) last = loose[j++].tick / tpb;
        std::vector<SeqNote> mine(loose.begin() + (long)i, loose.begin() + (long)j);
        for (auto& m : mine) m.tick -= first * tpb;
        const uint16_t bars = (uint16_t)std::min<uint32_t>(last - first + 1, 0xFFFF);
        SongClip c;
        c.bar = first;
        c.bars = bars;
        c.notes = makeSeqPattern(bars, std::move(mine));
        addClip(track, c);
        i = j;
    }
    return true;
}

void AudioSong::removeTrack(int track)
{
    if (track < 0 || track >= kTracks || !tracks[track].inUse) return;
    /* The ones below move up. Arrange draws the tracks in use as a list with
     * "+ ADD TRACK" under the last of them, and a hole in the middle of that
     * list has no meaning on screen -- track 2 of three, missing, would read
     * as a track that lost its audio. */
    for (int t = track; t + 1 < kTracks; ++t) tracks[t] = std::move(tracks[t + 1]);
    tracks[kTracks - 1] = SongTrack{};
}

} // namespace crosspad

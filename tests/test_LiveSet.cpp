// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include "crosspad/song/LiveSet.hpp"

#include <zlib.h>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace crosspad;

namespace {

std::string gunzipFile(const std::string& path)
{
    gzFile g = gzopen(path.c_str(), "rb");
    REQUIRE(g != nullptr);
    std::string out;
    char buf[65536];
    int n = 0;
    while ((n = gzread(g, buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
    const int err = n < 0 ? n : Z_OK;
    gzclose(g);
    REQUIRE(err == Z_OK);
    return out;
}

std::string skeleton()
{
    return gunzipFile(std::string(CROSSPAD_CORE_DIR) + "/data/live12_set.xml.gz");
}

std::string render(const AudioSong& song, const std::vector<LiveSetClipFile>& files, LiveSetReport& report)
{
    std::string xml;
    REQUIRE(writeLiveSetXml(skeleton(), song, files,
                            [&](const char* p, size_t n) { xml.append(p, n); return true; }, report));
    return xml;
}

/// The first @p attr value inside the first element on @p path.
std::string valueIn(const std::string& xml, std::vector<std::string> path, const std::string& attr)
{
    size_t a = 0, b = xml.size();
    for (const auto& tag : path) {
        a = xml.find("<" + tag, a);
        REQUIRE(a < b);
        b = xml.find("</" + tag + ">", a);
        REQUIRE(b != std::string::npos);
    }
    const size_t v = xml.find(attr + "=\"", a);
    REQUIRE(v < b);
    const size_t s = v + attr.size() + 2;
    return xml.substr(s, xml.find('"', s) - s);
}

/// Every AudioTrack element, in order.
std::vector<std::string> tracks(const std::string& xml)
{
    std::vector<std::string> out;
    for (size_t a = xml.find("<AudioTrack "); a != std::string::npos; a = xml.find("<AudioTrack ", a + 1)) {
        out.push_back(xml.substr(a, xml.find("</AudioTrack>", a) - a));
    }
    return out;
}

/// Each arrangement clip of @p track as attribute -> value of its direct fields.
std::vector<std::map<std::string, std::string>> clipsOf(const std::string& track)
{
    std::vector<std::map<std::string, std::string>> out;
    const size_t ms = track.find("<MainSequencer>");
    const size_t me = track.find("</MainSequencer>");
    for (size_t a = track.find("<AudioClip ", ms); a < me; a = track.find("<AudioClip ", a + 1)) {
        const std::string c = track.substr(a, track.find("</AudioClip>", a) - a);
        std::map<std::string, std::string> m;
        for (const char* k : {"CurrentStart", "CurrentEnd", "LoopEnd", "OutMarker", "HiddenLoopEnd", "Name",
                              "RelativePath", "DefaultDuration", "DefaultSampleRate", "RelativePathType"}) {
            const std::string key = std::string("<") + k + " Value=\"";
            const size_t v = c.find(key);
            if (v != std::string::npos) m[k] = c.substr(v + key.size(), c.find('"', v + key.size()) - v - key.size());
        }
        const size_t w = c.rfind("BeatTime=\"");
        m["WarpBeats"] = c.substr(w + 10, c.find('"', w + 10) - w - 10);
        out.push_back(m);
    }
    return out;
}

AudioSong song95()
{
    AudioSong s;
    s.name = "Soul vibe";
    s.bpm = 95.0f;
    s.beatsPerBar = 4;
    s.loop = true;
    return s;
}

/// A bounce: exactly @p bars on the engine's grid at @p bpm.
LiveSetClipFile bounce(const std::string& file, uint32_t bars, float bpm, uint32_t rate = 44100)
{
    LiveSetClipFile f;
    f.file = file;
    f.sampleRate = rate;
    f.frames = (uint32_t)(rate * 60.0f / bpm) * 4 * bars;
    f.bytes = 44 + f.frames * 4ull;
    f.modified = 1790526154;
    return f;
}

} // namespace

TEST_CASE("GzipStoredWriter: what zlib reads back, blocks over 64 kB included", "[song][liveset]")
{
    const std::string path = (fs::temp_directory_path() / "cp_liveset_gz.gz").string();
    std::string data;
    for (int i = 0; i < 200000; ++i) data += (char)('a' + (i * 7919) % 26);
    GzipStoredWriter w;
    REQUIRE(w.open(path));
    REQUIRE(w.write(data.data(), 5));
    REQUIRE(w.write(data.data() + 5, data.size() - 5));
    REQUIRE(w.close());
    REQUIRE(gunzipFile(path) == data);   // gzread checks the CRC and the length
    fs::remove(path);
}

TEST_CASE("Live Set: the tempo in the main mixer and in the envelope Live reads", "[song][liveset]")
{
    AudioSong s = song95();
    s.beatsPerBar = 3;
    LiveSetReport r;
    const std::string xml = render(s, {}, r);
    CHECK(valueIn(xml, {"MainTrack", "Mixer", "Tempo"}, "<Manual Value") == "95");
    const std::string tempoId = valueIn(xml, {"MainTrack", "Mixer", "Tempo"}, "<AutomationTarget Id");
    const size_t env = xml.find("<PointeeId Value=\"" + tempoId + "\"");
    REQUIRE(env != std::string::npos);
    const size_t ev = xml.find("<FloatEvent ", env);
    CHECK(xml.substr(xml.find("Value=\"", ev) + 7, 2) == "95");
    CHECK(valueIn(xml, {"MainTrack", "Mixer", "TimeSignature"}, "<Manual Value") == "200");   // 3/4
    const std::string sigId = valueIn(xml, {"MainTrack", "Mixer", "TimeSignature"}, "<AutomationTarget Id");
    const size_t sev = xml.find("<EnumEvent ", xml.find("<PointeeId Value=\"" + sigId + "\""));
    CHECK(xml.substr(xml.find("Value=\"", sev) + 7, 3) == "200");
}

TEST_CASE("Live Set: four tracks before the returns, every pointee id once", "[song][liveset]")
{
    AudioSong s = song95();
    s.tracks[0].clips.push_back({"a.wav", 0, 2});
    LiveSetReport r;
    const std::string xml = render(s, {bounce("a.wav", 2, 95.0f)}, r);
    const auto ts = tracks(xml);
    REQUIRE(ts.size() == 4);
    CHECK(xml.rfind("<AudioTrack ") < xml.find("<ReturnTrack "));
    for (int t = 0; t < 4; ++t) {
        CHECK(ts[t].find("<EffectiveName Value=\"Track " + std::to_string(t + 1) + "\"") != std::string::npos);
        CHECK(ts[t].find("<UserName Value=\"Track " + std::to_string(t + 1) + "\"") != std::string::npos);
    }

    /* The pointee namespace: tracks, Pointees and every *Target. */
    std::regex id("<(AudioTrack|ReturnTrack|Pointee|[A-Za-z]*Target) Id=\"([0-9]+)\"");
    std::set<unsigned long> seen;
    std::map<std::string, std::set<unsigned long>> perKind;
    unsigned long maxId = 0;
    for (auto it = std::sregex_iterator(xml.begin(), xml.end(), id); it != std::sregex_iterator(); ++it) {
        const std::string kind = (*it)[1] == "AudioTrack" || (*it)[1] == "ReturnTrack" ? "track" : "pointee";
        const unsigned long v = std::stoul((*it)[2]);
        INFO((*it)[0]);
        CHECK(perKind[kind].insert(v).second);
        maxId = std::max(maxId, v);
    }
    CHECK(std::stoul(valueIn(xml, {"LiveSet"}, "<NextPointeeId Value")) > maxId);

    /* ClipSlot ids are scene numbers, untouched in every clone. */
    for (const auto& t : ts) CHECK(t.find("<ClipSlot Id=\"0\">") != std::string::npos);
}

TEST_CASE("Live Set: blocks as the engine plays them", "[song][liveset]")
{
    AudioSong s = song95();
    auto& cs = s.tracks[1].clips;
    cs.push_back({"loop.wav", 0, 2});    // exactly two bars: plays whole
    cs.push_back({"long.wav", 2, 4});    // cut by the next block at bar 4
    cs.push_back({"short.wav", 4, 2});   // one bar made at 95, in a two-bar block
    cs.push_back({"slow.wav", 6, 2});    // two bars at 88 BPM: longer than its block, cut at its end
    cs.push_back({"gone.wav", 8, 1});
    s.tracks[2].clips.push_back({"loop.wav", 0, 2});
    s.tracks[2].clips.push_back({"loop.wav", 0, 2});   // an old song's duplicate
    s.tracks[3].clips.push_back({"a.wav", 0, 2});
    s.tracks[3].clips.push_back({"b.wav", 0, 2});      // the later one on a bar is the one heard

    const std::vector<LiveSetClipFile> files = {
        bounce("loop.wav", 2, 95.0f), bounce("long.wav", 4, 95.0f), bounce("short.wav", 1, 95.0f),
        bounce("slow.wav", 2, 88.0f), bounce("a.wav", 2, 95.0f), bounce("b.wav", 2, 95.0f),
    };
    LiveSetReport r;
    const std::string xml = render(s, files, r);
    const auto ts = tracks(xml);

    const auto t2 = clipsOf(ts[1]);
    REQUIRE(t2.size() == 4);
    CHECK(t2[0].at("CurrentStart") == "0");
    CHECK(t2[0].at("CurrentEnd") == "8");
    CHECK(t2[0].at("LoopEnd") == "8");
    CHECK(t2[0].at("WarpBeats") == "8");
    CHECK(t2[0].at("Name") == "loop");
    CHECK(t2[0].at("RelativePathType") == "3");
    CHECK(t2[0].at("RelativePath") == "clips/loop.wav");
    CHECK(t2[0].at("DefaultSampleRate") == "44100");
    CHECK(t2[0].at("DefaultDuration") == std::to_string(files[0].frames));

    CHECK(t2[1].at("CurrentStart") == "8");
    CHECK(t2[1].at("CurrentEnd") == "16");        // two bars, then the next block
    CHECK(t2[1].at("OutMarker") == "8");
    CHECK(t2[1].at("LoopEnd") == "16");           // the rest stays in the clip, to drag back out
    CHECK(t2[1].at("HiddenLoopEnd") == "16");

    CHECK(t2[2].at("CurrentStart") == "16");
    CHECK(t2[2].at("CurrentEnd") == "20");        // the file ends; so does the clip
    CHECK(t2[2].at("WarpBeats") == "4");

    /* Made at 88: 8 beats of 88 are 8 * 95/88 beats of 95, never stretched. */
    const double slow = std::stod(t2[3].at("WarpBeats"));
    CHECK(std::abs(slow - (double)files[3].frames / 44100.0 * 95.0 / 60.0) < 1e-9);
    CHECK(slow > 8.6);
    CHECK(t2[3].at("CurrentEnd") == "32");

    CHECK(clipsOf(ts[2]).size() == 1);            // the duplicate once
    const auto t4 = clipsOf(ts[3]);
    REQUIRE(t4.size() == 1);
    CHECK(t4[0].at("Name") == "b");

    CHECK(r.clips == 6);
    std::string notes;
    for (const auto& n : r.notes) notes += n + "\n";
    INFO(notes);
    CHECK(r.notes.size() == 2);
    CHECK(notes.find("gone.wav: not in clips/") != std::string::npos);
    CHECK(notes.find("a.wav is under b.wav") != std::string::npos);

    /* Clips on one track never overlap. */
    for (const auto& t : ts) {
        double end = -1.0;
        for (const auto& c : clipsOf(t)) {
            CHECK(std::stod(c.at("CurrentStart")) >= end);
            end = std::stod(c.at("CurrentEnd"));
        }
    }
}

TEST_CASE("Live Set: faders, mutes, loop region, names that need escaping", "[song][liveset]")
{
    AudioSong s = song95();
    s.tracks[0].volume = 0.707946f;
    s.tracks[1].volume = 3.0f;
    s.tracks[2].muted = true;
    s.tracks[0].clips.push_back({"R&B \"take\".wav", 0, 8});
    s.loopStart = 2;
    s.loopEnd = 6;
    LiveSetReport r;
    const std::string xml = render(s, {bounce("R&B \"take\".wav", 8, 95.0f)}, r);
    const auto ts = tracks(xml);
    CHECK(valueIn(ts[0], {"Mixer", "Volume"}, "<Manual Value") == "0.707946");
    CHECK(valueIn(ts[1], {"Mixer", "Volume"}, "<Manual Value") == "1.995262");
    CHECK(valueIn(ts[2], {"Mixer", "Speaker"}, "<Manual Value") == "false");
    CHECK(valueIn(ts[3], {"Mixer", "Speaker"}, "<Manual Value") == "true");
    CHECK(valueIn(xml, {"Transport"}, "<LoopOn Value") == "true");
    CHECK(valueIn(xml, {"Transport"}, "<LoopStart Value") == "8");
    CHECK(valueIn(xml, {"Transport"}, "<LoopLength Value") == "16");
    CHECK(ts[0].find("<Name Value=\"R&amp;B &quot;take&quot;\" />") != std::string::npos);
    CHECK(ts[0].find("clips/R&amp;B &quot;take&quot;.wav") != std::string::npos);
}

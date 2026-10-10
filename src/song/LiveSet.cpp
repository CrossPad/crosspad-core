// SPDX-License-Identifier: GPL-3.0-or-later
//
// An AudioSong as an Ableton Live 12 Set. Live's own empty set is the
// template (data/live12_set.xml.gz): its one AudioTrack is cloned per song
// track, and the few values that carry the song -- names, faders, mutes,
// tempo, loop, clips -- are written into it by finding the element that
// holds them. Everything else stays as Live wrote it.

#include "crosspad/song/LiveSet.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace crosspad {

namespace {

constexpr size_t kNpos = std::string::npos;
/// Above every id the skeleton uses (it tops out at 22181).
constexpr uint32_t kFirstId = 30000;
/// Live's fader at +6 dB, its top; song volumes are linear gain like it.
constexpr float kMaxVolume = 1.995262f;
/// A file within this fraction of a whole number of beats is taken to be
/// that long: a bounce is exactly its bars on the engine's grid, whose beat
/// is a whole number of samples, a hair short of the song's.
constexpr double kSameLength = 0.001;
/// Live palette indices, one per track.
constexpr int kColors[AudioSong::kTracks] = {4, 1, 8, 11};

std::string num(double v)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%.15g", v);
    return b;
}

std::string numf(float v)
{
    char b[24];
    std::snprintf(b, sizeof(b), "%.7g", (double)v);
    return b;
}

std::string escape(const std::string& s)
{
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        default:  o += c;
        }
    }
    return o;
}

/// The first @p what in [from, to) of @p s, or kNpos.
size_t find(const std::string& s, const std::string& what, size_t from, size_t to)
{
    const size_t p = s.find(what, from);
    return p != kNpos && p + what.size() <= to ? p : kNpos;
}

/// Narrow [a, b) to the inside of the first <tag>...</tag> in it, a tag at a
/// time: {"MainTrack", "Mixer", "Tempo"}. An opening tag is matched with its
/// attributes or without; the first closing tag after it ends it (none of
/// the paths walked here nest an element in one of its own name).
bool narrow(const std::string& s, std::initializer_list<const char*> path, size_t& a, size_t& b)
{
    for (const char* tag : path) {
        const std::string open = std::string("<") + tag;
        size_t p = a;
        for (;;) {
            p = find(s, open, p, b);
            if (p == kNpos) return false;
            const char next = s[p + open.size()];
            if (next == '>' || next == ' ') break;
            p += open.size();
        }
        const size_t e = find(s, std::string("</") + tag + ">", p, b);
        if (e == kNpos) return false;
        a = p;
        b = e;
    }
    return true;
}

/// Where the value of the first @p attr (`<Manual Value="`) in [a, b) is.
bool valueAt(const std::string& s, const std::string& attr, size_t a, size_t b, size_t& v, size_t& len)
{
    const size_t p = find(s, attr, a, b);
    if (p == kNpos) return false;
    v = p + attr.size();
    const size_t q = s.find('"', v);
    if (q == kNpos || q > b) return false;
    len = q - v;
    return true;
}

bool setValue(std::string& s, std::initializer_list<const char*> path, const std::string& attr,
              const std::string& value)
{
    size_t a = 0, b = s.size(), v = 0, len = 0;
    if (!narrow(s, path, a, b) || !valueAt(s, attr, a, b, v, len)) return false;
    s.replace(v, len, value);
    return true;
}

std::string getValue(const std::string& s, std::initializer_list<const char*> path, const std::string& attr)
{
    size_t a = 0, b = s.size(), v = 0, len = 0;
    if (!narrow(s, path, a, b) || !valueAt(s, attr, a, b, v, len)) return {};
    return s.substr(v, len);
}

/// In the main track's envelopes, set the first @p event ("<FloatEvent ")
/// of the envelope on @p pointee to @p value.
bool setEnvelope(std::string& s, const std::string& pointee, const char* event, const std::string& value)
{
    size_t a = 0, b = s.size();
    if (pointee.empty() || !narrow(s, {"MainTrack", "AutomationEnvelopes"}, a, b)) return false;
    const std::string key = "<PointeeId Value=\"" + pointee + "\"";
    const size_t k = find(s, key, a, b);
    if (k == kNpos) return false;
    const size_t end = find(s, "</AutomationEnvelope>", k, b);
    const size_t e = find(s, event, k, end);
    size_t v = 0, len = 0;
    if (e == kNpos || !valueAt(s, " Value=\"", e, end, v, len)) return false;
    s.replace(v, len, value);
    return true;
}

/// Ids in one namespace across the whole Set; every other Id is a counter
/// local to its list and must keep its value (a ClipSlot's is its scene).
bool globalTag(const std::string& s, size_t idAttr)
{
    const size_t lt = s.rfind('<', idAttr);
    if (lt == kNpos) return false;
    size_t e = s.find_first_of(" />", lt + 1);
    if (e == kNpos || e > idAttr) e = idAttr;
    const std::string tag = s.substr(lt + 1, e - lt - 1);
    static const std::string kTarget = "Target";
    return tag == "AudioTrack" || tag == "Pointee" ||
           (tag.size() >= kTarget.size() && tag.compare(tag.size() - kTarget.size(), kTarget.size(), kTarget) == 0);
}

/// @p each(position of the digits, their length) for every global Id.
template <typename F> void eachGlobalId(const std::string& s, size_t a, size_t b, F each)
{
    static const std::string kId = " Id=\"";
    for (size_t p = find(s, kId, a, b); p != kNpos; p = find(s, kId, p + kId.size(), b)) {
        const size_t v = p + kId.size();
        const size_t q = s.find('"', v);
        if (q == kNpos || q > b) return;
        if (globalTag(s, p)) each(v, q - v);
    }
}

/// The track at [a, b) of @p skel with every global id renumbered from
/// @p nextId on, one number per distinct old one.
std::string cloneTrack(const std::string& skel, size_t a, size_t b, uint32_t& nextId)
{
    std::string out;
    out.reserve(b - a + 64);
    std::map<unsigned long, uint32_t> ids;
    size_t at = a;
    eachGlobalId(skel, a, b, [&](size_t v, size_t len) {
        out.append(skel, at, v - at);
        const unsigned long old = std::strtoul(skel.c_str() + v, nullptr, 10);
        auto it = ids.find(old);
        if (it == ids.end()) it = ids.emplace(old, nextId++).first;
        out += std::to_string(it->second);
        at = v + len;
    });
    out.append(skel, at, b - at);
    return out;
}

/// One block as an arrangement clip. Positions are in beats.
struct Placed {
    const LiveSetClipFile* file = nullptr;
    double start = 0.0;    ///< on the song
    double played = 0.0;   ///< how much of the clip sounds
    double skip = 0.0;     ///< where in the file it starts: a cut's second half
    double length = 0.0;   ///< the whole file at the song's tempo
};

/// An arrangement clip as Live 12.4 saves one, with the fields a block
/// changes in braces. Positions are in beats; the file is relative to the
/// project folder (RelativePathType 3), and its absolute Path is the same
/// relative one, since where the card is mounted is not known here. The
/// warp markers say seconds of file = beats of song: the clip's own tempo is
/// the song's, so it plays at the speed it was recorded, as on the CrossPad.
constexpr const char* kClip = R"(<AudioClip Id="{id}" Time="{start}">
	<LomId Value="0" />
	<LomIdView Value="0" />
	<CurrentStart Value="{start}" />
	<CurrentEnd Value="{end}" />
	<Loop>
		<LoopStart Value="{skip}" />
		<LoopEnd Value="{full}" />
		<StartRelative Value="0" />
		<LoopOn Value="false" />
		<OutMarker Value="{out}" />
		<HiddenLoopStart Value="{skip}" />
		<HiddenLoopEnd Value="{full}" />
	</Loop>
	<Name Value="{name}" />
	<Annotation Value="" />
	<Color Value="{color}" />
	<LaunchMode Value="0" />
	<LaunchQuantisation Value="0" />
	<TimeSignature>
		<TimeSignatures>
			<RemoteableTimeSignature Id="0">
				<Numerator Value="{bpb}" />
				<Denominator Value="4" />
				<Time Value="0" />
			</RemoteableTimeSignature>
		</TimeSignatures>
	</TimeSignature>
	<Envelopes>
		<Envelopes />
	</Envelopes>
	<ScrollerTimePreserver>
		<LeftTime Value="0" />
		<RightTime Value="{full}" />
	</ScrollerTimePreserver>
	<TimeSelection>
		<AnchorTime Value="0" />
		<OtherTime Value="0" />
	</TimeSelection>
	<Legato Value="false" />
	<Ram Value="false" />
	<GrooveSettings>
		<GrooveId Value="-1" />
	</GrooveSettings>
	<Disabled Value="false" />
	<VelocityAmount Value="0" />
	<FollowAction>
		<FollowTime Value="4" />
		<IsLinked Value="true" />
		<LoopIterations Value="1" />
		<FollowActionA Value="4" />
		<FollowActionB Value="0" />
		<FollowChanceA Value="100" />
		<FollowChanceB Value="0" />
		<JumpIndexA Value="1" />
		<JumpIndexB Value="1" />
		<FollowActionEnabled Value="false" />
	</FollowAction>
	<Grid>
		<FixedNumerator Value="1" />
		<FixedDenominator Value="16" />
		<GridIntervalPixel Value="20" />
		<Ntoles Value="2" />
		<SnapToGrid Value="true" />
		<Fixed Value="false" />
	</Grid>
	<FreezeStart Value="0" />
	<FreezeEnd Value="0" />
	<IsWarped Value="true" />
	<TakeId Value="1" />
	<IsInKey Value="false" />
	<ScaleInformation>
		<Root Value="0" />
		<Name Value="0" />
	</ScaleInformation>
	<AutomationEnvelopesListWrapper LomId="0" />
	<SampleRef>
		<FileRef>
			<RelativePathType Value="3" />
			<RelativePath Value="{path}" />
			<Path Value="{path}" />
			<Type Value="2" />
			<OriginalFileSize Value="{bytes}" />
			<OriginalCrc Value="0" />
		</FileRef>
		<LastModDate Value="{modified}" />
		<SampleUsageHint Value="0" />
		<DefaultDuration Value="{frames}" />
		<DefaultSampleRate Value="{rate}" />
		<SamplesToAutoWarp Value="0" />
	</SampleRef>
	<Onsets>
		<UserOnsets />
		<HasUserOnsets Value="false" />
	</Onsets>
	<WarpMode Value="0" />
	<GranularityTones Value="30" />
	<GranularityTexture Value="65" />
	<FluctuationTexture Value="25" />
	<TransientResolution Value="6" />
	<TransientLoopMode Value="2" />
	<TransientEnvelope Value="100" />
	<ComplexProFormants Value="100" />
	<ComplexProEnvelope Value="128" />
	<Sync Value="true" />
	<HiQ Value="true" />
	<Fade Value="false" />
	<Fades>
		<FadeInLength Value="0" />
		<FadeOutLength Value="0" />
		<ClipFadesAreInitialized Value="true" />
		<CrossfadeInState Value="0" />
		<FadeInCurveSkew Value="0" />
		<FadeInCurveSlope Value="0" />
		<FadeOutCurveSkew Value="0" />
		<FadeOutCurveSlope Value="0" />
		<IsDefaultFadeIn Value="false" />
		<IsDefaultFadeOut Value="false" />
	</Fades>
	<PitchCoarse Value="0" />
	<PitchFine Value="0" />
	<SampleVolume Value="1" />
	<WarpMarkers>
		<WarpMarker Id="0" SecTime="0" BeatTime="0" />
		<WarpMarker Id="1" SecTime="{seconds}" BeatTime="{full}" />
	</WarpMarkers>
	<SavedWarpMarkersForStretched />
	<MarkersGenerated Value="true" />
	<IsSongTempoLeader Value="false" />
</AudioClip>
)";

/// kClip filled in for one block, each line indented by @p ind.
void clipXml(std::string& o, const std::string& ind, int id, const Placed& c, int color, uint8_t bpb)
{
    const LiveSetClipFile& f = *c.file;
    const size_t dot = f.file.find_last_of('.');
    const std::pair<const char*, std::string> fields[] = {
        {"id", std::to_string(id)},
        {"start", num(c.start)},
        {"end", num(c.start + c.played)},
        {"full", num(c.length)},
        {"skip", num(c.skip)},
        {"out", num(c.skip + c.played)},
        {"name", escape(dot == kNpos ? f.file : f.file.substr(0, dot))},
        {"color", std::to_string(color)},
        {"bpb", std::to_string(bpb)},
        {"path", escape(std::string(AudioSong::clipsDir()) + "/" + f.file)},
        {"bytes", std::to_string(f.bytes)},
        {"modified", std::to_string(f.modified)},
        {"frames", std::to_string(f.frames)},
        {"rate", std::to_string(f.sampleRate)},
        {"seconds", num((double)f.frames / f.sampleRate)},
    };
    bool lineStart = true;
    for (const char* t = kClip; *t; ++t) {
        if (lineStart) o += ind;
        lineStart = *t == '\n';
        if (*t != '{') { o += *t; continue; }
        const char* close = std::strchr(t, '}');
        for (const auto& fl : fields) {
            if (std::strncmp(t + 1, fl.first, (size_t)(close - t - 1)) == 0 && !fl.first[close - t - 1]) o += fl.second;
        }
        t = close;
    }
}

/// The blocks of @p track as clips, laid out the way the song engine plays
/// them (SongEngine's flatten()): a block sounds from its bar until its end,
/// the next block's bar or the song's end, whichever comes first.
std::vector<Placed> place(const AudioSong& song, int track, const std::vector<LiveSetClipFile>& files,
                          double bpm, std::set<std::string>& unreadable, LiveSetReport& report)
{
    std::vector<Placed> out;
    /* A seq track's blocks are its scene, not files: Live gets the audio. */
    if (song.tracks[track].kind == TrackKind::Seq) return out;
    const auto& cs = song.tracks[track].clips;
    const double bpb = song.beatsPerBar;
    const uint32_t cut = std::max(song.playEnd(), song.hasRegion() ? song.loopEnd : 0u);
    for (size_t i = 0; i < cs.size(); ++i) {
        const SongClip& c = cs[i];
        uint32_t end = std::min(c.bar + c.bars, cut);
        if (i + 1 < cs.size()) end = std::min(end, cs[i + 1].bar);
        if (end <= c.bar) {
            const SongClip* next = i + 1 < cs.size() ? &cs[i + 1] : nullptr;
            const std::string where = "Track " + std::to_string(track + 1) + ", bar " + std::to_string(c.bar + 1) + ": ";
            if (c.bar >= cut) {
                report.notes.push_back(where + c.file + " is after the song's end, left out");
            } else if (next && !(next->file == c.file && next->bars == c.bars && next->bar == c.bar)) {
                report.notes.push_back(where + c.file + " is under " + next->file + ", left out");
            }
            continue;
        }
        auto f = std::find_if(files.begin(), files.end(), [&](const LiveSetClipFile& x) { return x.file == c.file; });
        if (f == files.end() || !f->frames || !f->sampleRate) {
            if (unreadable.insert(c.file).second) {
                report.notes.push_back(c.file + (f == files.end() ? ": not in clips/, left out"
                                                                  : ": empty or not a WAV, left out"));
            }
            continue;
        }
        Placed p;
        p.file = &*f;
        p.start = c.bar * bpb;
        p.length = (double)f->frames / f->sampleRate * bpm / 60.0;
        const double whole = std::round(p.length);
        if (whole >= 1.0 && std::fabs(p.length - whole) <= whole * kSameLength) p.length = whole;
        p.skip = (double)c.offset / f->sampleRate * bpm / 60.0;
        double rest = std::max(0.0, p.length - p.skip);
        if (c.frames) rest = std::min(rest, (double)c.frames / f->sampleRate * bpm / 60.0);
        p.played = std::min((end - c.bar) * bpb, rest);
        out.push_back(p);
    }
    return out;
}

/// @p v as `<Name Value="v" />` holds it: the XML's own escaping.
bool setName(std::string& t, const std::string& name)
{
    return setValue(t, {"Name"}, "<EffectiveName Value=\"", name) &&
           setValue(t, {"Name"}, "<UserName Value=\"", name);
}

uint32_t crc32(uint32_t crc, const uint8_t* p, size_t n)
{
    static const uint32_t kNibble[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ kNibble[crc & 15];
        crc = (crc >> 4) ^ kNibble[crc & 15];
    }
    return ~crc;
}

void le32(uint8_t* b, uint32_t v)
{
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}

} // namespace

bool writeLiveSetXml(std::string skel, const AudioSong& song, const std::vector<LiveSetClipFile>& files,
                     const std::function<bool(const char*, size_t)>& out, LiveSetReport& report)
{
    report = LiveSetReport{};
    const uint8_t bpb = song.beatsPerBar ? song.beatsPerBar : AudioSong::kDefaultBeatsPerBar;
    const double bpm = song.bpm > 0.0f ? song.bpm : AudioSong::kDefaultBpm;

    /* The song-wide values first, while the skeleton is still one string:
     * tempo and metre live in the main track's mixer and again in its
     * envelopes, and Live reads the envelopes -- set only the mixer and the
     * Set opens at 120. */
    const std::string tempoId = getValue(skel, {"MainTrack", "Mixer", "Tempo"}, "<AutomationTarget Id=\"");
    const std::string sigId = getValue(skel, {"MainTrack", "Mixer", "TimeSignature"}, "<AutomationTarget Id=\"");
    const std::string sig = std::to_string(198 + (bpb - 1));   // 99 * log2(4) + numerator - 1
    const uint32_t len = song.hasRegion() ? song.loopEnd - song.loopStart : song.playEnd();
    bool ok = setValue(skel, {"MainTrack", "Mixer", "Tempo"}, "<Manual Value=\"", numf((float)bpm)) &&
              setValue(skel, {"MainTrack", "Mixer", "TimeSignature"}, "<Manual Value=\"", sig) &&
              setEnvelope(skel, tempoId, "<FloatEvent ", numf((float)bpm)) &&
              setEnvelope(skel, sigId, "<EnumEvent ", sig) &&
              setValue(skel, {"Transport"}, "<LoopOn Value=\"", song.loop ? "true" : "false");
    if (ok && len) {
        ok = setValue(skel, {"Transport"}, "<LoopStart Value=\"", num((song.hasRegion() ? song.loopStart : 0) * bpb)) &&
             setValue(skel, {"Transport"}, "<LoopLength Value=\"", num(len * bpb));
    }
    if (!ok) return false;

    /* The one AudioTrack, whole lines: the clones go where it was, before
     * the returns. */
    size_t ta = 0, tb = skel.size();
    if (!narrow(skel, {"Tracks", "AudioTrack"}, ta, tb)) return false;
    ta = skel.rfind('\n', ta) + 1;
    tb = skel.find('\n', tb);
    if (tb == kNpos) return false;
    ++tb;

    uint32_t maxId = 0, perTrack = 0;
    std::set<unsigned long> protoIds;
    eachGlobalId(skel, 0, skel.size(), [&](size_t v, size_t) {
        const unsigned long id = std::strtoul(skel.c_str() + v, nullptr, 10);
        maxId = std::max<uint32_t>(maxId, (uint32_t)id);
        if (v >= ta && v < tb) protoIds.insert(id);
    });
    perTrack = (uint32_t)protoIds.size();
    uint32_t nextId = std::max(kFirstId, maxId + 1);
    if (!setValue(skel, {"LiveSet"}, "<NextPointeeId Value=\"",
                  std::to_string(nextId + perTrack * AudioSong::kTracks))) {
        return false;
    }
    /* That edit is before the track: find it again. */
    ta = 0;
    tb = skel.size();
    if (!narrow(skel, {"Tracks", "AudioTrack"}, ta, tb)) return false;
    ta = skel.rfind('\n', ta) + 1;
    tb = skel.find('\n', tb) + 1;

    if (!out(skel.data(), ta)) return false;
    std::set<std::string> unreadable;
    for (int t = 0; t < AudioSong::kTracks; ++t) {
        const SongTrack& tr = song.tracks[t];
        std::string track = cloneTrack(skel, ta, tb, nextId);
        const std::vector<Placed> clips = place(song, t, files, bpm, unreadable, report);
        std::string events;
        size_t ea = 0, eb = track.size();
        if (!narrow(track, {"MainSequencer", "Sample", "ArrangerAutomation"}, ea, eb)) return false;
        const size_t ev = find(track, "<Events />", ea, eb);
        if (ev == kNpos) return false;
        const size_t line = track.rfind('\n', ev) + 1;
        const std::string ind = track.substr(line, ev - line);
        for (size_t i = 0; i < clips.size(); ++i) clipXml(events, ind + "\t", (int)i, clips[i], kColors[t], bpb);
        report.clips = (uint16_t)(report.clips + clips.size());
        if (!clips.empty()) track.replace(ev, std::strlen("<Events />"), "<Events>\n" + events + ind + "</Events>");
        const std::string name = "Track " + std::to_string(t + 1);
        if (!setName(track, name) ||
            !setValue(track, {"AudioTrack"}, "<Color Value=\"", std::to_string(kColors[t])) ||
            !setValue(track, {"Mixer", "Volume"}, "<Manual Value=\"", numf(std::min(std::max(tr.volume, 0.0f), kMaxVolume))) ||
            !setValue(track, {"Mixer", "Speaker"}, "<Manual Value=\"", tr.muted ? "false" : "true") ||
            !out(track.data(), track.size())) {
            return false;
        }
    }
    return out(skel.data() + tb, skel.size() - tb);
}

bool GzipStoredWriter::open(const std::string& path)
{
    if (f_) std::fclose(f_);
    f_ = std::fopen(path.c_str(), "wb");
    crc_ = 0;
    size_ = 0;
    /* No name, no time, "unknown" OS: 10 bytes. */
    static const uint8_t kHeader[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff};
    ok_ = f_ && std::fwrite(kHeader, 1, sizeof(kHeader), f_) == sizeof(kHeader);
    return ok_;
}

bool GzipStoredWriter::write(const char* data, size_t n)
{
    const auto* p = reinterpret_cast<const uint8_t*>(data);
    while (ok_ && n) {
        const uint16_t len = (uint16_t)std::min<size_t>(n, 0xFFFF);
        const uint8_t head[5] = {0, (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)~len, (uint8_t)(~len >> 8)};
        ok_ = std::fwrite(head, 1, sizeof(head), f_) == sizeof(head) && std::fwrite(p, 1, len, f_) == len;
        crc_ = crc32(crc_, p, len);
        size_ += len;
        p += len;
        n -= len;
    }
    return ok_;
}

bool GzipStoredWriter::close()
{
    if (!f_) return false;
    uint8_t tail[13] = {1, 0, 0, 0xff, 0xff};   // an empty final block
    le32(tail + 5, crc_);
    le32(tail + 9, size_);
    ok_ = ok_ && std::fwrite(tail, 1, sizeof(tail), f_) == sizeof(tail);
    ok_ = std::fclose(f_) == 0 && ok_;
    f_ = nullptr;
    return ok_;
}

} // namespace crosspad

// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/kit/KitRateConvert.hpp"

#include "crosspad/dsp/FftResampler.hpp"
#include "crosspad/kit/KitPathUtils.hpp"
#include "crosspad/sampler/SampleStore.hpp"
#include "../util/JsonStdio.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <new>

#include <sys/stat.h>

#ifdef _WIN32
#include <filesystem>
#else
#include <dirent.h>
#include <unistd.h>
#endif

namespace crosspad {

namespace {

constexpr uint32_t kChunkFrames = 1024;          // frames read per step
constexpr uint32_t kMaxScaledChunk = 64 * 1024;  // a metadata chunk read whole; larger is dropped
constexpr uint64_t kSpaceMargin = 256 * 1024;    // beyond the files themselves
/* Everything below comes off a card anyone can write to, so every size it
 * states is bounded before it is used. */
constexpr uint64_t kMaxOutBytes = 0x7FFF0000u;   // a converted file's samples: under every 32-bit size it has
constexpr long     kMaxJsonBytes = 1024 * 1024;  // a kit's JSON
constexpr long     kMaxListBytes = 64 * 1024;    // the tidy's list
constexpr size_t   kMaxListLines = 1024;
constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatExtensible = 0xFFFE;
const char* const kJournal = ".originals";

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}
void wr16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
void wr32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24;
}
bool idIs(const char id[4], const char* tag) { return std::memcmp(id, tag, 4) == 0; }

/// @p v at @p inRate as the same moment at @p outRate, rounded, clamped to
/// 32 bits: upsampling a position near the top of the range does not wrap.
uint32_t scale64(uint64_t v, uint32_t inRate, uint32_t outRate) {
    if (!inRate || inRate == outRate) return v > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(v);
    const uint64_t r = (v * outRate + inRate / 2) / inRate;   // v < 2^33, outRate < 2^31: no overflow
    return r > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(r);
}

/// A length or a position, rounded as scaleFrame() does.
uint32_t scaleU32At(uint8_t* p, uint32_t inRate, uint32_t outRate) {
    const uint32_t v = scaleFrame(rd32(p), inRate, outRate);
    wr32(p, v);
    return v;
}

bool pathExists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

uint32_t fileSize(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 ? static_cast<uint32_t>(st.st_size) : 0;
}

std::string parentOf(const std::string& p) {
    const auto s = p.rfind('/');
    return s == std::string::npos ? std::string() : p.substr(0, s);
}

std::string baseOf(const std::string& p) {
    const auto s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

/// FAT compares names without case: so does every path match here.
bool sameName(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
            return false;
    }
    return *a == *b;
}

/// "a/b/../c/./d" -> "a/c/d". A leading '/' is kept.
std::string normalizePath(const std::string& p) {
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= p.size()) {
        const size_t j = std::min(p.find('/', i), p.size());
        const std::string part = p.substr(i, j - i);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        i = j + 1;
    }
    std::string out = (!p.empty() && p[0] == '/') ? "/" : "";
    for (size_t k = 0; k < parts.size(); ++k) out += (k ? "/" : "") + parts[k];
    return out;
}

/// @p path is @p dir or below it -- @p path normalised, @p dir already so.
bool isUnder(const std::string& path, const std::string& dir) {
    return path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 && path[dir.size()] == '/';
}

/// A sample of a kit, as a card path with every "." and ".." resolved: the
/// only form any check below is made on.
std::string sampleFsPath(const std::string& kitJsonPath, const char* sample) {
    return normalizePath(kitFsPath(resolveKitSamplePath(kitJsonPath, sample)));
}

bool hasWavSuffix(const std::string& p) {
    return p.size() > 4 && sameName(p.c_str() + p.size() - 4, ".wav");
}

bool makeDirs(IKitConvertHost& host, const std::string& path, const std::string& floor) {
    if (path.size() <= floor.size()) return true;
    if (pathExists(path)) return true;
    if (!makeDirs(host, parentOf(path), floor)) return false;
    return host.makeDir(path) || pathExists(path);
}

/// Every sample string of a kit document: the pads' layers and a pitched
/// kit's zones. @p fn(pad, layer object or null, the string).
template <typename Fn>
void forEachSample(JsonDocument& doc, Fn fn) {
    JsonArray pads = doc["pads"].as<JsonArray>();
    int padIdx = 0;
    for (JsonObject pad : pads) {
        JsonArray samples = pad["samples"].as<JsonArray>();
        for (JsonVariant sv : samples) {
            if (sv.is<const char*>()) {
                fn(padIdx, sv, JsonObject(), sv.as<const char*>());
            } else if (sv.is<JsonObject>()) {
                JsonObject o = sv.as<JsonObject>();
                const char* s = o["sample"] | "";
                if (s[0]) fn(padIdx, sv, o, s);
            }
        }
        ++padIdx;
    }
    JsonArray zones = doc["instrument"]["zones"].as<JsonArray>();
    for (JsonObject z : zones) {
        const char* s = z["sample"] | "";
        if (s[0]) fn(-1, JsonVariant(), z, s);
    }
}

bool readJson(const std::string& fsPath, JsonDocument& doc, const JsonDocument* filter = nullptr) {
    FILE* f = std::fopen(fsPath.c_str(), "r");
    // What writeJson() leaves when a save is cut between its two renames.
    if (!f && errno == ENOENT) f = std::fopen((fsPath + ".bak").c_str(), "r");
    if (!f) return false;
    // A kit is a few kB; a document of megabytes is not one, and parsing it
    // would take the PSRAM the UI lives in.
    const bool sane = std::fseek(f, 0, SEEK_END) == 0 && std::ftell(f) >= 0 &&
                      std::ftell(f) <= kMaxJsonBytes && std::fseek(f, 0, SEEK_SET) == 0;
    if (!sane) { std::fclose(f); return false; }
    JsonFileReader in(f);
    const DeserializationError err = filter
        ? deserializeJson(doc, in, DeserializationOption::Filter(*filter))
        : deserializeJson(doc, in);
    std::fclose(f);
    return !err;
}

/// Through `.tmp` and `.bak`, as PortableKitLoader saves a kit: a failed
/// write costs the write, never the file already there.
bool writeJson(const std::string& fsPath, const JsonDocument& doc) {
    const std::string tmp = fsPath + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return false;
    JsonFileWriter out{f};
    serializeJson(doc, out);
    if (!closeWritten(f)) { std::remove(tmp.c_str()); return false; }
    const std::string bak = fsPath + ".bak";
    bool hadOld = false;
    if (pathExists(fsPath)) {
        std::remove(bak.c_str());
        hadOld = std::rename(fsPath.c_str(), bak.c_str()) == 0;
    }
    if (std::rename(tmp.c_str(), fsPath.c_str()) != 0) {
        if (hadOld) std::rename(bak.c_str(), fsPath.c_str());
        std::remove(tmp.c_str());
        return false;
    }
    std::remove(bak.c_str());
    return true;
}

/// Lines of a small text file; empty when it is not there.
std::vector<std::string> readLines(const std::string& path) {
    std::vector<std::string> out;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return out;
    std::string line;
    int c;
    long read = 0;
    while (read++ < kMaxListBytes && out.size() < kMaxListLines && (c = std::fgetc(f)) != EOF) {
        if (c == '\n') { if (!line.empty()) out.push_back(line); line.clear(); }
        else if (c != '\r') line += static_cast<char>(c);
    }
    if (!line.empty() && out.size() < kMaxListLines) out.push_back(line);
    std::fclose(f);
    return out;
}

/// The list replaced whole, or removed when it is empty.
bool writeLines(const std::string& path, const std::vector<std::string>& lines) {
    if (lines.empty()) {
        std::remove(path.c_str());
        return !pathExists(path);
    }
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return false;
    for (const auto& l : lines) std::fprintf(f, "%s\n", l.c_str());
    if (!closeWritten(f)) { std::remove(tmp.c_str()); return false; }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

/// Directories under @p dir, hidden ones left out.
std::vector<std::string> subDirs(const std::string& dir) {
    std::vector<std::string> out;
#ifdef _WIN32
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::u8path(dir), ec)) {
        const std::string n = e.path().filename().u8string();
        if (!n.empty() && n[0] != '.' && e.is_directory(ec)) out.push_back(n);
    }
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] != '.') names.emplace_back(e->d_name);
    }
    closedir(d);
    for (auto& n : names) {
        struct stat st;
        if (stat((dir + "/" + n).c_str(), &st) == 0 && S_ISDIR(st.st_mode)) out.push_back(std::move(n));
    }
#endif
    return out;
}

/// Every kit JSON under @p dirFs, found as the kit scan finds them: a folder
/// with kit.json or <folder>.json is a kit and is not looked into further.
template <typename Fn>
void forEachKitJson(const std::string& dirFs, Fn fn, int depth = 0) {
    if (depth > 8) return;
    for (const std::string& name : subDirs(dirFs)) {
        const std::string sub = dirFs + "/" + name;
        const std::string a = sub + "/kit.json";
        const std::string b = sub + "/" + name + ".json";
        if (pathExists(a) || pathExists(a + ".bak")) fn(a);
        else if (pathExists(b) || pathExists(b + ".bak")) fn(b);
        else forEachKitJson(sub, fn, depth + 1);
    }
}

void removeEmptyDir(const std::string& path) {
#ifdef _WIN32
    std::error_code ec;
    if (std::filesystem::is_empty(std::filesystem::u8path(path), ec)) {
        std::filesystem::remove(std::filesystem::u8path(path), ec);
    }
#else
    rmdir(path.c_str());   // fails, harmlessly, while anything is in it
#endif
}

/// FftResamplerStream takes plain function pointers; the conversion running
/// is the one whose host they reach. One runs at a time.
IKitConvertHost* g_allocHost = nullptr;
void* hostAlloc(size_t n) { return g_allocHost ? g_allocHost->alloc(n) : nullptr; }
void  hostFree(void* p) { if (g_allocHost) g_allocHost->release(p); }

/// The host's transform, its block capped at kConvertFftMax.
class CappedFft : public IFft {
public:
    explicit CappedFft(IFft& base) : base_(base) {}
    bool forward(float* d, uint32_t n) override { return n <= maxSize() && base_.forward(d, n); }
    bool inverse(float* d, uint32_t n) override { return n <= maxSize() && base_.inverse(d, n); }
    uint32_t maxSize() const override { return std::min(base_.maxSize(), kConvertFftMax); }
private:
    IFft& base_;
};

void setText(char* dst, size_t cap, const char* s) {
    std::snprintf(dst, cap, "%s", s ? s : "");
}

} // namespace

// ── Small pieces ────────────────────────────────────────────────────────────

std::string rateFolderName(uint32_t rate) {
    const uint32_t k = rate / 1000;
    uint32_t frac = rate % 1000;
    std::string s = std::to_string(k) + "k";
    if (frac) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%03u", static_cast<unsigned>(frac));
        std::string f = buf;
        while (!f.empty() && f.back() == '0') f.pop_back();
        s += f;
    }
    return s;
}

uint32_t scaleFrame(uint32_t frame, uint32_t inRate, uint32_t outRate) {
    return scale64(frame, inRate, outRate);
}

bool WavLayout::pcm16() const {
    return format == kFormatPcm && bits == 16 && (channels == 1 || channels == 2) && rate > 0 && frames > 0;
}

bool WavLayout::parse(FILE* f) {
    *this = WavLayout();
    if (!f || std::fseek(f, 0, SEEK_END) != 0) return false;
    const long end = std::ftell(f);
    if (end < 12) return false;
    fileBytes = static_cast<uint32_t>(end);
    uint8_t h[12];
    if (std::fseek(f, 0, SEEK_SET) != 0 || std::fread(h, 1, 12, f) != 12) return false;
    if (std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVE", 4) != 0) return false;
    const uint32_t riff = rd32(h + 4);
    riffBytes = riff > UINT32_MAX - 8 ? UINT32_MAX : riff + 8;

    bool haveFmt = false, haveData = false;
    uint32_t pos = 12;
    for (int guard = 0; guard < 256 && pos + 8 <= fileBytes; ++guard) {
        uint8_t ch[8];
        if (std::fseek(f, pos, SEEK_SET) != 0 || std::fread(ch, 1, 8, f) != 8) break;
        const uint32_t size = rd32(ch + 4);
        const uint32_t avail = fileBytes - (pos + 8);
        if (std::memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fm[40] = {};
            const uint32_t want = std::min<uint32_t>(size, sizeof(fm));
            if (want < 16 || std::fread(fm, 1, want, f) != want) return false;
            format = rd16(fm);
            channels = rd16(fm + 2);
            rate = rd32(fm + 4);
            bits = rd16(fm + 14);
            // WAVE_FORMAT_EXTENSIBLE: the real format is the subformat GUID's first two bytes.
            if (format == kFormatExtensible && want >= 26) format = rd16(fm + 24);
            haveFmt = true;
        } else if (std::memcmp(ch, "data", 4) == 0) {
            dataOffset = pos + 8;
            dataBytes = std::min(size, avail);
            haveData = true;
        } else {
            Chunk c;
            std::memcpy(c.id, ch, 4);
            c.offset = pos + 8;
            c.size = std::min(size, avail);
            c.afterData = haveData;
            chunks.push_back(c);
        }
        if (size > avail) break;                // runs past the file: nothing after it
        pos += 8 + size + (size & 1);           // chunks are word aligned
    }
    if (!haveFmt || !haveData) return false;
    const uint32_t frameBytes = static_cast<uint32_t>(channels) * (bits / 8);
    frames = frameBytes ? dataBytes / frameBytes : 0;
    return true;
}

bool wavChunkScaled(const char id[4]) {
    return idIs(id, "smpl") || idIs(id, "cue ") || idIs(id, "LIST") || idIs(id, "bext") || idIs(id, "fact");
}

static bool wavChunkDropped(const char id[4]) {
    return idIs(id, "PEAK") || idIs(id, "JUNK") || idIs(id, "junk") || idIs(id, "PAD ") || idIs(id, "FLLR");
}

bool scaleWavChunk(const char id[4], uint8_t* p, uint32_t size, uint32_t inRate, uint32_t outRate) {
    if (wavChunkDropped(id)) return false;
    if (idIs(id, "smpl") && size >= 36) {
        wr32(p + 8, static_cast<uint32_t>((1000000000ull + outRate / 2) / outRate));   // ns per sample
        const uint32_t loops = std::min(rd32(p + 28), (size - 36) / 24);
        for (uint32_t i = 0; i < loops; ++i) {
            uint8_t* l = p + 36 + 24 * i;
            const uint32_t start = scaleU32At(l + 8, inRate, outRate);
            // The end is the loop's last frame, inclusive: scaled as the frame
            // after it, so the loop keeps its length. Never before the start.
            const uint32_t endPlus = scale64(static_cast<uint64_t>(rd32(l + 12)) + 1, inRate, outRate);
            wr32(l + 12, endPlus > start ? endPlus - 1 : start);
        }
    } else if (idIs(id, "cue ") && size >= 4) {
        const uint32_t n = std::min(rd32(p), (size - 4) / 24);
        for (uint32_t i = 0; i < n; ++i) {
            uint8_t* c = p + 4 + 24 * i;
            scaleU32At(c + 4, inRate, outRate);    // dwPosition
            scaleU32At(c + 20, inRate, outRate);   // dwSampleOffset
        }
    } else if (idIs(id, "LIST") && size >= 4 && std::memcmp(p, "adtl", 4) == 0) {
        // at <= size throughout, and every pass moves it on by 8 or more:
        // a sub-chunk size is compared with the room left, never added to
        // the cursor first (it wrapped, wrote past the buffer and never moved
        // again).
        uint32_t at = 4;
        while (size - at >= 8) {
            const uint32_t sz = rd32(p + at + 4);
            if (sz > size - at - 8) break;                     // runs past the list: nothing after it is read
            if (std::memcmp(p + at, "ltxt", 4) == 0 && sz >= 8) {
                scaleU32At(p + at + 8 + 4, inRate, outRate);   // dwSampleLength
            }
            at += 8 + sz;
            if ((sz & 1) && at < size) ++at;
        }
    } else if (idIs(id, "bext") && size >= 346) {
        const uint64_t ref = static_cast<uint64_t>(rd32(p + 338)) | static_cast<uint64_t>(rd32(p + 342)) << 32;
        // Seconds since midnight in samples: anything past a day is not a time
        // reference, and is left as it is rather than overflow.
        const uint64_t scaled = (inRate && ref < (UINT64_MAX - inRate) / outRate)
                                    ? (ref * outRate + inRate / 2) / inRate : ref;
        wr32(p + 338, static_cast<uint32_t>(scaled));
        wr32(p + 342, static_cast<uint32_t>(scaled >> 32));
    } else if (idIs(id, "fact") && size >= 4) {
        scaleU32At(p, inRate, outRate);
    }
    return true;
}

// ── Host defaults ───────────────────────────────────────────────────────────

void* IKitConvertHost::alloc(size_t bytes) { return std::malloc(bytes); }
void  IKitConvertHost::release(void* p) { std::free(p); }

size_t IKitConvertHost::readIo(FILE* f, void* dst, size_t bytes) { return std::fread(dst, 1, bytes, f); }

bool IKitConvertHost::writeIo(FILE* f, const void* src, size_t bytes) {
    return std::fwrite(src, 1, bytes, f) == bytes;
}

bool IKitConvertHost::syncFile(FILE* f) { return std::fflush(f) == 0; }

uint64_t IKitConvertHost::freeBytes(const std::string& fsDir) {
    (void)fsDir;
    return UINT64_MAX;
}

bool IKitConvertHost::makeDir(const std::string& fsPath) {
#ifdef _WIN32
    std::error_code ec;
    return std::filesystem::create_directory(std::filesystem::u8path(fsPath), ec);
#else
    return mkdir(fsPath.c_str(), 0755) == 0;
#endif
}

// ── The converter ───────────────────────────────────────────────────────────

/// One file on its way: both handles, a resampler per channel and the
/// buffers between them, all in one allocation from the host.
struct KitRateConverter::Io {
    explicit Io(IFft& base) : fft(base) {}

    CappedFft fft;
    FILE* src = nullptr;
    FILE* dst = nullptr;
    WavLayout lay;
    std::string part;
    FftResamplerStream rs[2];
    void* mem = nullptr;
    int16_t* in = nullptr;        ///< kChunkFrames interleaved frames
    int16_t* chIn[2] = {};        ///< kChunkFrames each
    int16_t* chOut[2] = {};       ///< outCap each
    int16_t* out = nullptr;       ///< outCap interleaved frames
    uint32_t outCap = 0;
    uint32_t inPos = 0;           ///< source frames consumed
    uint32_t outTotal = 0;        ///< frames the converted file holds
    uint32_t outWritten = 0;
    uint32_t dataSizeAt = 0;      ///< where the data chunk's size goes
};

KitRateConverter::~KitRateConverter() { endFileIo(); }

uint8_t KitRateConverter::Progress::percent() const {
    switch (phase) {
    case Phase::Idle:       return 0;
    case Phase::Planning:   return 1;
    case Phase::Converting:
        return static_cast<uint8_t>(2 + (framesTotal ? static_cast<uint64_t>(framesDone) * 96 / framesTotal : 96));
    case Phase::Committing: return 99;
    default:                return 100;
    }
}

uint32_t KitRateConverter::addName(const std::string& s) {
    const uint32_t off = static_cast<uint32_t>(names_.size());
    names_ += s;
    names_ += '\0';
    return off;
}

bool KitRateConverter::fail(const char* why) {
    dropFile();
    setText(progress_.error, sizeof(progress_.error), why);
    progress_.phase = Phase::Failed;
    return false;
}

bool KitRateConverter::begin(IKitConvertHost& host, const std::string& kitJsonPath, uint32_t targetRate) {
    endFileIo();
    host_ = &host;
    g_allocHost = &host;
    kitJson_ = kitJsonPath;
    kitDirFs_ = normalizePath(parentOf(kitFsPath(kitJsonPath)));
    target_ = targetRate;
    names_.clear();
    files_.clear();
    edits_.clear();
    cursor_ = 0;
    kitRead_ = false;
    progress_ = Progress();
    progress_.toRate = targetRate;
    cancel_.store(false, std::memory_order_relaxed);
    if (kitJsonPath.empty() || kitDirFs_.empty() || targetRate == 0) return fail("no kit");
    progress_.phase = Phase::Planning;
    return true;
}

bool KitRateConverter::planKit() {
    JsonDocument doc(host_->jsonAllocator() ? host_->jsonAllocator()
                                            : ArduinoJson::detail::DefaultAllocator::instance());
    if (!readJson(kitFsPath(kitJson_), doc)) return fail("cannot read the kit");
    if (std::strcmp(doc["engine"] | "", "pitched") == 0) return fail("pitched kit");

    const std::string folder = rateFolderName(target_);
    const std::string folderFs = kitDirFs_ + "/" + folder;
    std::vector<uint32_t> rels;   // for the name check of borrowed files
    forEachSample(doc, [&](int pad, JsonVariant, JsonObject, const char* s) {
        if (pad < 0) return;
        // Resolved, then every ".." taken out, before anything is decided on
        // it: "../../x.wav" begins with the kit's folder as text and is not in it.
        const std::string fs = sampleFsPath(kitJson_, s);
        if (fs.empty() || baseOf(fs).empty()) return;
        for (const FileJob& f : files_) if (sameName(name(f.src), fs.c_str())) return;
        FileJob f;
        std::string rel;
        if (isUnder(fs, kitDirFs_)) {
            f.inside = true;
            rel = folder + "/" + fs.substr(kitDirFs_.size() + 1);
        } else {
            // Borrowed from elsewhere on the card: copied in, never touched.
            const std::string base = baseOf(fs);
            const auto dot = base.rfind('.');
            const std::string stem = dot == std::string::npos ? base : base.substr(0, dot);
            const std::string ext = dot == std::string::npos ? "" : base.substr(dot);
            for (int n = 1;; ++n) {
                rel = folder + "/_shared/" + (n == 1 ? base : stem + "_" + std::to_string(n) + ext);
                bool taken = false;
                for (uint32_t r : rels) taken = taken || sameName(name(r), rel.c_str());
                if (!taken) break;
            }
        }
        // Whatever the names were, the file written is in the rate's folder.
        const std::string dst = normalizePath(kitDirFs_ + "/" + rel);
        if (!isUnder(dst, folderFs)) return;
        f.src = addName(fs);
        f.rel = addName(rel);
        rels.push_back(f.rel);
        f.dst = addName(dst);
        files_.push_back(f);
    });
    progress_.files = static_cast<uint16_t>(files_.size());
    return true;
}

bool KitRateConverter::planFile(FileJob& f) {
    f.described = true;
    FILE* in = std::fopen(name(f.src), "rb");
    WavLayout lay;
    const bool ok = in && lay.parse(in);
    if (in) std::fclose(in);
    if (!ok || !lay.pcm16()) {
        f.state = FileState::Skip;
        ++progress_.skipped;
        return true;
    }
    f.rate = lay.rate;
    f.frames = lay.frames;
    f.channels = static_cast<uint8_t>(lay.channels);
    f.srcBytes = lay.fileBytes;
    if (lay.rate == target_) {
        f.state = FileState::Skip;   // already at the rate: nothing to do, nothing wrong
        return true;
    }
    // Converted, it has to fit every 32-bit size a WAV has, with room for its
    // metadata: an 8 kHz file of 900 MB would be 5 GB at 44.1 kHz.
    const uint64_t outFrames = (static_cast<uint64_t>(lay.frames) * target_ + lay.rate / 2) / lay.rate;
    if (outFrames * lay.channels * 2 > kMaxOutBytes) {
        f.state = FileState::Skip;
        ++progress_.skipped;
        return true;
    }
    // The store keys a sample on its path: one that does not fit is a pad
    // that would never load. Room for ".part" too, while it is written.
    if (std::strlen(name(f.dst)) + 6 >= SampleStore::kMaxPath) {
        f.state = FileState::Skip;
        ++progress_.skipped;
        return true;
    }
    ++progress_.toConvert;

    // Converted before (a run that was stopped, or cut): kept if it verifies.
    FILE* out = std::fopen(name(f.dst), "rb");
    WavLayout done;
    const bool have = out && done.parse(out);
    if (out) std::fclose(out);
    if (have && done.pcm16() && done.rate == target_ && done.channels == f.channels &&
        done.frames == resampledLength(f.frames, f.rate, target_) && done.riffBytes == done.fileBytes) {
        f.state = FileState::Done;
        ++progress_.converted;
        return true;
    }
    progress_.framesTotal += f.frames;
    return true;
}

bool KitRateConverter::planDone() {
    // The rate most of them were at, for the words on the screen.
    uint32_t best = 0, bestCount = 0;
    for (const FileJob& a : files_) {
        if (!a.rate || a.rate == target_) continue;
        uint32_t n = 0;
        for (const FileJob& b : files_) n += b.rate == a.rate;
        if (n > bestCount) { best = a.rate; bestCount = n; }
    }
    progress_.fromRate = best;

    uint64_t need = 0;
    for (const FileJob& f : files_) {
        if (f.state != FileState::Pending) continue;
        const uint64_t pcm = static_cast<uint64_t>(f.frames) * f.channels * 2;
        const uint64_t meta = f.srcBytes > pcm ? f.srcBytes - pcm : 0;
        need += static_cast<uint64_t>(resampledLength(f.frames, f.rate, target_)) * f.channels * 2 + meta;
    }
    if (need) {
        const uint64_t free = host_->freeBytes(kitDirFs_);
        if (free != UINT64_MAX && free < need + kSpaceMargin) {
            char why[48];
            std::snprintf(why, sizeof(why), "card full: needs %u MB",
                          static_cast<unsigned>((need + kSpaceMargin + 1048575) / 1048576));
            return fail(why);
        }
    }
    cursor_ = 0;
    progress_.phase = Phase::Converting;
    return true;
}

void KitRateConverter::endFileIo() {
    if (!io_) return;
    io_->rs[0].end();
    io_->rs[1].end();
    if (io_->src) std::fclose(io_->src);
    if (io_->dst) std::fclose(io_->dst);
    if (io_->mem && host_) host_->release(io_->mem);
    delete io_;
    io_ = nullptr;
}

void KitRateConverter::dropFile() {
    if (!io_) return;
    const std::string part = io_->part;
    endFileIo();
    if (!part.empty()) std::remove(part.c_str());
}

/// Copy one metadata chunk from the source, scaled when it carries positions.
/// @p piece holds @p pieceCap bytes: every read and write is a piece of at
/// most that, and of kConvertPieceBytes.
static bool copyChunk(IKitConvertHost& host, FILE* src, FILE* dst, const WavLayout::Chunk& c,
                      uint32_t inRate, uint32_t outRate, uint8_t* piece, uint32_t pieceCap) {
    const uint32_t step = std::min(pieceCap, kConvertPieceBytes);
    if (step == 0) return false;
    uint8_t hdr[8];
    std::memcpy(hdr, c.id, 4);
    wr32(hdr + 4, c.size);
    const uint8_t pad = 0;
    if (wavChunkScaled(c.id)) {
        if (c.size > kMaxScaledChunk) return true;               // dropped: too big to rewrite
        uint8_t* buf = static_cast<uint8_t*>(host.alloc(c.size ? c.size : 1));
        if (!buf) return false;
        bool read = std::fseek(src, c.offset, SEEK_SET) == 0;
        for (uint32_t at = 0; read && at < c.size; at += step) {
            const uint32_t n = std::min(step, c.size - at);
            read = host.readIo(src, buf + at, n) == n;
        }
        bool ok = read;
        if (read && scaleWavChunk(c.id, buf, c.size, inRate, outRate)) {
            ok = host.writeIo(dst, hdr, 8);
            for (uint32_t at = 0; ok && at < c.size; at += step)
                ok = host.writeIo(dst, buf + at, std::min(step, c.size - at));
            if (ok && (c.size & 1)) ok = host.writeIo(dst, &pad, 1);
        }
        host.release(buf);
        return ok;
    }
    if (wavChunkDropped(c.id)) return true;
    if (std::fseek(src, c.offset, SEEK_SET) != 0 || !host.writeIo(dst, hdr, 8)) return false;
    for (uint32_t at = 0; at < c.size;) {
        const uint32_t n = std::min(step, c.size - at);
        if (host.readIo(src, piece, n) != n || !host.writeIo(dst, piece, n)) return false;
        at += n;
        host.giveWay();
    }
    return !(c.size & 1) || host.writeIo(dst, &pad, 1);
}

bool KitRateConverter::startFile() {
    FileJob& f = files_[cursor_];
    ++progress_.fileIndex;
    setText(progress_.current, sizeof(progress_.current), baseOf(name(f.src)).c_str());

    const std::string dst = name(f.dst);
    if (!makeDirs(*host_, parentOf(dst), kitDirFs_)) return fail("cannot make the folder");

    io_ = new (std::nothrow) Io(host_->fft());
    if (!io_) return fail("not enough memory");
    Io& io = *io_;
    io.src = std::fopen(name(f.src), "rb");
    if (!io.src || !io.lay.parse(io.src) || !io.lay.pcm16() || io.lay.rate != f.rate ||
        io.lay.frames != f.frames || io.lay.channels != f.channels) {
        // Changed or gone since the plan: left as it is.
        endFileIo();
        f.state = FileState::Skip;
        ++progress_.skipped;
        --progress_.toConvert;
        progress_.framesTotal -= std::min(progress_.framesTotal, f.frames);
        return true;
    }

    const uint32_t ch = f.channels;
    const double ratio = static_cast<double>(target_) / f.rate;
    /* What one push can hand back: its own frames at the ratio, plus up to two
     * analysis blocks the flush finishes, plus the stream's slack. */
    const uint32_t block = kConvertFftMax / kConvertOversample;
    io.outCap = static_cast<uint32_t>((kChunkFrames + 2 * block) * ratio) + 2 * FftResamplerStream::kStreamOutSlack;
    const size_t samples = static_cast<size_t>(kChunkFrames) * ch      // in
                         + static_cast<size_t>(kChunkFrames) * ch      // chIn
                         + static_cast<size_t>(io.outCap) * ch         // chOut
                         + static_cast<size_t>(io.outCap) * ch;        // out
    io.mem = host_->alloc(samples * sizeof(int16_t));
    if (!io.mem) return fail("not enough memory");
    int16_t* p = static_cast<int16_t*>(io.mem);
    io.in = p;                         p += kChunkFrames * ch;
    for (uint32_t c = 0; c < ch; ++c) { io.chIn[c] = p; p += kChunkFrames; }
    for (uint32_t c = 0; c < ch; ++c) { io.chOut[c] = p; p += io.outCap; }
    io.out = p;
    for (uint32_t c = 0; c < ch; ++c) {
        if (!io.rs[c].begin(io.fft, f.rate, target_, hostAlloc, hostFree, kConvertOversample))
            return fail("not enough memory");
    }
    io.outTotal = resampledLength(f.frames, f.rate, target_);

    io.part = dst + ".part";
    std::remove(io.part.c_str());
    io.dst = std::fopen(io.part.c_str(), "wb");
    if (!io.dst) return fail("cannot write to the card");

    uint8_t h[36];
    std::memcpy(h, "RIFF", 4);
    wr32(h + 4, 0);                               // patched when the file is done
    std::memcpy(h + 8, "WAVEfmt ", 8);
    wr32(h + 16, 16);
    wr16(h + 20, kFormatPcm);
    wr16(h + 22, static_cast<uint16_t>(ch));
    wr32(h + 24, target_);
    wr32(h + 28, target_ * ch * 2);
    wr16(h + 32, static_cast<uint16_t>(ch * 2));
    wr16(h + 34, 16);
    if (!host_->writeIo(io.dst, h, sizeof(h))) return fail("cannot write to the card");
    for (const auto& c : io.lay.chunks) {
        if (c.afterData) continue;
        if (!copyChunk(*host_, io.src, io.dst, c, f.rate, target_, reinterpret_cast<uint8_t*>(io.out),
                       io.outCap * ch * 2))
            return fail("cannot write to the card");
    }
    uint8_t d[8];
    std::memcpy(d, "data", 4);
    wr32(d + 4, io.outTotal * ch * 2);
    const long at = std::ftell(io.dst);
    if (at < 0 || !host_->writeIo(io.dst, d, 8)) return fail("cannot write to the card");
    io.dataSizeAt = static_cast<uint32_t>(at) + 4;
    if (std::fseek(io.src, io.lay.dataOffset, SEEK_SET) != 0) return fail("cannot read a sample");
    return true;
}

/// @p got frames per channel in io.chOut, interleaved and written -- never
/// past the length the file is to have.
static bool emit(IKitConvertHost& host, FILE* dst, int16_t* const* chOut,
                 int16_t* out, uint32_t ch, uint32_t got, uint32_t& written, uint32_t total) {
    if (written + got > total) got = total - written;
    for (uint32_t i = 0; i < got; ++i)
        for (uint32_t c = 0; c < ch; ++c) out[i * ch + c] = chOut[c][i];
    const uint8_t* b = reinterpret_cast<const uint8_t*>(out);
    const uint32_t bytes = got * ch * 2;
    for (uint32_t at = 0; at < bytes; at += kConvertPieceBytes) {
        if (!host.writeIo(dst, b + at, std::min<uint32_t>(kConvertPieceBytes, bytes - at))) return false;
    }
    written += got;
    return true;
}

bool KitRateConverter::stepFile() {
    Io& io = *io_;
    FileJob& f = files_[cursor_];
    const uint32_t ch = f.channels;
    const uint32_t n = std::min(kChunkFrames, f.frames - io.inPos);
    const uint32_t bytes = n * ch * 2;
    uint8_t* b = reinterpret_cast<uint8_t*>(io.in);
    for (uint32_t at = 0; at < bytes;) {
        const uint32_t want = std::min<uint32_t>(kConvertPieceBytes, bytes - at);
        if (host_->readIo(io.src, b + at, want) != want) return fail("cannot read a sample");
        at += want;
    }
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t c = 0; c < ch; ++c) io.chIn[c][i] = io.in[i * ch + c];
    uint32_t got = UINT32_MAX;
    for (uint32_t c = 0; c < ch; ++c) got = std::min(got, io.rs[c].push(io.chIn[c], n, io.chOut[c], io.outCap));
    if (!io.rs[0].ok() || (ch == 2 && !io.rs[1].ok())) return fail("resampler failed");
    if (!emit(*host_, io.dst, io.chOut, io.out, ch, got, io.outWritten, io.outTotal))
        return fail("cannot write to the card");
    io.inPos += n;
    progress_.framesDone += n;
    host_->giveWay();
    return io.inPos < f.frames ? true : finishFile();
}

bool KitRateConverter::finishFile() {
    Io& io = *io_;
    FileJob& f = files_[cursor_];
    const uint32_t ch = f.channels;
    uint32_t got = UINT32_MAX;
    for (uint32_t c = 0; c < ch; ++c) got = std::min(got, io.rs[c].flush(io.chOut[c], io.outCap));
    if (!emit(*host_, io.dst, io.chOut, io.out, ch, got, io.outWritten, io.outTotal))
        return fail("cannot write to the card");
    // A stream that came up short: the rest is the silence past the end.
    while (io.outWritten < io.outTotal) {
        const uint32_t n = std::min(io.outCap, io.outTotal - io.outWritten);
        for (uint32_t c = 0; c < ch; ++c) std::memset(io.chOut[c], 0, n * sizeof(int16_t));
        if (!emit(*host_, io.dst, io.chOut, io.out, ch, n, io.outWritten, io.outTotal))
            return fail("cannot write to the card");
    }
    for (const auto& c : io.lay.chunks) {
        if (!c.afterData) continue;
        if (!copyChunk(*host_, io.src, io.dst, c, f.rate, target_, reinterpret_cast<uint8_t*>(io.out),
                       io.outCap * ch * 2))
            return fail("cannot write to the card");
    }
    const long end = std::ftell(io.dst);
    uint8_t v[4];
    wr32(v, static_cast<uint32_t>(end) - 8);
    bool ok = end > 0 && std::fseek(io.dst, 4, SEEK_SET) == 0 && host_->writeIo(io.dst, v, 4);
    wr32(v, io.outTotal * ch * 2);
    ok = ok && std::fseek(io.dst, io.dataSizeAt, SEEK_SET) == 0 && host_->writeIo(io.dst, v, 4);
    ok = ok && host_->syncFile(io.dst);
    ok = std::fclose(io.dst) == 0 && ok;
    io.dst = nullptr;
    std::fclose(io.src);
    io.src = nullptr;
    if (!ok) { dropFile(); return fail("cannot write to the card"); }

    // Read back before it counts: a file the card did not keep is not swapped in.
    FILE* chk = std::fopen(io.part.c_str(), "rb");
    WavLayout back;
    const bool parsed = chk && back.parse(chk);
    if (chk) std::fclose(chk);
    if (!parsed || !back.pcm16() || back.rate != target_ || back.channels != ch ||
        back.frames != io.outTotal || back.riffBytes != back.fileBytes) {
        dropFile();
        return fail("verify failed");
    }
    const std::string dst = name(f.dst);
    std::remove(dst.c_str());                    // a stale one that did not verify
    if (std::rename(io.part.c_str(), dst.c_str()) != 0) { dropFile(); return fail("cannot write to the card"); }
    io.part.clear();
    endFileIo();
    f.state = FileState::Done;
    ++progress_.converted;
    ++cursor_;
    return true;
}

bool KitRateConverter::commit() {
    bool any = false;
    for (FileJob& f : files_) {
        if (f.state != FileState::Done) continue;
        // Replaced on the card while it was being converted: the kit keeps it.
        if (fileSize(name(f.src)) != f.srcBytes) { f.state = FileState::Skip; ++progress_.skipped; continue; }
        any = true;
    }
    if (!any) { progress_.phase = Phase::Done; return false; }

    JsonDocument doc(host_->jsonAllocator() ? host_->jsonAllocator()
                                            : ArduinoJson::detail::DefaultAllocator::instance());
    const std::string jsonFs = kitFsPath(kitJson_);
    if (!readJson(jsonFs, doc)) return fail("cannot read the kit");

    forEachSample(doc, [&](int pad, JsonVariant v, JsonObject o, const char* s) {
        if (pad < 0) return;
        const std::string fs = sampleFsPath(kitJson_, s);
        for (const FileJob& f : files_) {
            if (f.state != FileState::Done || !sameName(name(f.src), fs.c_str())) continue;
            KitConvertEdit e;
            e.pad = static_cast<uint8_t>(pad);
            e.oldSample = s;
            e.newSample = name(f.rel);
            if (o.isNull()) {
                v.set(e.newSample);
            } else {
                e.oldStart = o["start"] | static_cast<uint32_t>(0);
                e.oldEnd = o["end"] | static_cast<uint32_t>(0);
                e.start = scaleFrame(e.oldStart, f.rate, target_);
                e.end = scaleFrame(e.oldEnd, f.rate, target_);
                o["sample"] = e.newSample;
                if (!o["start"].isNull()) o["start"] = e.start;
                if (!o["end"].isNull()) o["end"] = e.end;
            }
            edits_.push_back(std::move(e));
            return;
        }
    });
    if (edits_.empty()) { progress_.phase = Phase::Done; return false; }

    // What the tidy is to remove, written before the kit stops naming it: a
    // cut between the two leaves a list whose files the kit still names, and
    // the tidy keeps those.
    const std::string folderFs = kitDirFs_ + "/" + rateFolderName(target_);
    const std::string journal = folderFs + "/" + kJournal;
    std::vector<std::string> lines = readLines(journal);
    for (const FileJob& f : files_) {
        if (f.state != FileState::Done || !f.inside) continue;
        const std::string rel = std::string(name(f.src)).substr(kitDirFs_.size() + 1);
        bool listed = false;
        for (const auto& l : lines) listed = listed || sameName(l.c_str(), rel.c_str());
        if (!listed) lines.push_back(rel);
    }
    if (!makeDirs(*host_, folderFs, kitDirFs_) || !writeLines(journal, lines)) {
        edits_.clear();
        return fail("cannot write to the card");
    }
    if (!writeJson(jsonFs, doc)) {
        edits_.clear();
        return fail("cannot save the kit");
    }
    progress_.phase = Phase::Done;
    return false;
}

bool KitRateConverter::step() {
    const bool more = stepOnce();
    if (!more && host_) host_->endJob();
    return more;
}

bool KitRateConverter::stepOnce() {
    switch (progress_.phase) {
    case Phase::Planning:
        if (cancel_.load(std::memory_order_relaxed)) { progress_.phase = Phase::Cancelled; return false; }
        if (!kitRead_) {
            kitRead_ = true;
            if (!planKit()) return false;
            if (files_.empty()) { progress_.phase = Phase::Done; return false; }
            return true;
        }
        if (cursor_ < files_.size()) { planFile(files_[cursor_++]); return true; }
        return planDone();

    case Phase::Converting:
        if (cancel_.load(std::memory_order_relaxed)) {
            dropFile();
            progress_.phase = Phase::Cancelled;
            return false;
        }
        if (!io_) {
            while (cursor_ < files_.size() && files_[cursor_].state != FileState::Pending) ++cursor_;
            if (cursor_ >= files_.size()) { progress_.phase = Phase::Committing; return true; }
            if (!startFile()) { dropFile(); return false; }
            if (!io_) ++cursor_;      // skipped: changed since the plan
            return true;
        }
        if (!stepFile()) { dropFile(); return false; }
        return true;

    case Phase::Committing:
        if (cancel_.load(std::memory_order_relaxed)) { progress_.phase = Phase::Cancelled; return false; }
        return commit();

    default:
        return false;
    }
}

// ── After the commit ────────────────────────────────────────────────────────

bool kitTidyPending(const std::string& kitJsonPath, uint32_t targetRate) {
    const std::string dir = parentOf(kitFsPath(kitJsonPath));
    return pathExists(dir + "/" + rateFolderName(targetRate) + "/" + kJournal);
}

KitTidyResult tidyKit(IKitConvertHost& host, const std::string& kitJsonPath,
                      const std::string& kitsRoot, uint32_t targetRate) {
    KitTidyResult r;
    const std::string jsonFs = kitFsPath(kitJsonPath);
    const std::string kitDirFs = normalizePath(parentOf(jsonFs));
    const std::string journal = kitDirFs + "/" + rateFolderName(targetRate) + "/" + kJournal;
    std::vector<std::string> lines = readLines(journal);
    if (lines.empty()) { r.ok = !pathExists(journal) || writeLines(journal, lines); return r; }

    /* The list is a file on the card, and a card is anyone's to write: a line
     * is an original only if it names a WAV in the kit's own folder -- not
     * above it, not the kit's JSON, not the converted files themselves. Any
     * other line is dropped from the list and nothing is done with it. */
    const std::string folderFs = kitDirFs + "/" + rateFolderName(targetRate);
    std::vector<std::string> paths;   // card paths of the originals
    std::vector<bool> named(lines.size(), false);
    std::vector<bool> refused(lines.size(), false);
    for (size_t i = 0; i < lines.size(); ++i) {
        paths.push_back(normalizePath(kitDirFs + "/" + lines[i]));
        refused[i] = lines[i][0] == '/' || !isUnder(paths[i], kitDirFs) || isUnder(paths[i], folderFs) ||
                     !hasWavSuffix(paths[i]);
    }

    ArduinoJson::Allocator* alloc = host.jsonAllocator() ? host.jsonAllocator()
                                                         : ArduinoJson::detail::DefaultAllocator::instance();
    const auto mark = [&](const std::string& fs) {
        for (size_t i = 0; i < paths.size(); ++i) named[i] = named[i] || sameName(paths[i].c_str(), fs.c_str());
    };
    {
        JsonDocument doc(alloc);
        if (!readJson(jsonFs, doc)) return r;      // not ok: nothing removed
        forEachSample(doc, [&](int, JsonVariant, JsonObject, const char* s) {
            mark(sampleFsPath(kitJsonPath, s));
        });
    }

    JsonDocument filter(alloc);
    filter["pads"][0]["samples"] = true;
    filter["instrument"]["zones"][0]["sample"] = true;
    forEachKitJson(kitFsPath(kitsRoot), [&](const std::string& otherFs) {
        if (sameName(otherFs.c_str(), jsonFs.c_str())) return;
        JsonDocument doc(alloc);
        if (!readJson(otherFs, doc, &filter)) return;
        const std::string otherDirFs = parentOf(otherFs);
        forEachSample(doc, [&](int, JsonVariant, JsonObject, const char* s) {
            // A bare name or a path below the other kit stays in that kit's
            // folder; only these two can reach into this one.
            if (s[0] == '/') mark(normalizePath(kitFsPath(s)));
            else if (std::strstr(s, "..")) mark(normalizePath(otherDirFs + "/" + s));
        });
        host.giveWay();
    });

    std::vector<std::string> left;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (named[i] || refused[i]) { ++r.kept; continue; }
        if (host.releaseFile(paths[i]) && (std::remove(paths[i].c_str()) == 0 || !pathExists(paths[i]))) {
            ++r.removed;
            const std::string dir = parentOf(paths[i]);
            if (dir.size() > kitDirFs.size()) removeEmptyDir(dir);
        } else {
            ++r.pending;
            left.push_back(lines[i]);
        }
    }
    r.ok = writeLines(journal, left);
    return r;
}

bool setKitKeepRate(IKitConvertHost& host, const std::string& kitJsonPath, bool keep) {
    JsonDocument doc(host.jsonAllocator() ? host.jsonAllocator()
                                          : ArduinoJson::detail::DefaultAllocator::instance());
    const std::string fs = kitFsPath(kitJsonPath);
    if (!readJson(fs, doc)) return false;
    if (keep) doc["keep_rate"] = true;
    else doc.remove("keep_rate");
    return writeJson(fs, doc);
}

} // namespace crosspad

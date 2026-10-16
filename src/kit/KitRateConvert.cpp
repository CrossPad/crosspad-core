// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/kit/KitRateConvert.hpp"

#include "crosspad/dsp/FftResampler.hpp"
#include "crosspad/kit/KitInfo.hpp"
#include "crosspad/kit/KitPathUtils.hpp"
#include "crosspad/sampler/SampleStore.hpp"
#include "KitJsonParser.hpp"
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
constexpr long     kMaxScanBytes = 256 * 1024;   // another kit's JSON, searched as text
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

/* Paths are handled in fixed buffers: every one a kit names fits
 * SampleStore::kMaxPath, and a buffer is no allocation to fail or to come
 * out of internal RAM. */
constexpr size_t kPath = 192;

bool pathExists(const char* p) {
    struct stat st;
    return stat(p, &st) == 0;
}

bool isDirPath(const char* p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

uint32_t fileSize(const char* p) {
    struct stat st;
    return stat(p, &st) == 0 ? static_cast<uint32_t>(st.st_size) : 0;
}

const char* baseOf(const char* p) {
    const char* s = std::strrchr(p, '/');
    return s ? s + 1 : p;
}

/// @p p without its last component, in place.
void cutLast(char* p) {
    char* s = std::strrchr(p, '/');
    if (s) *s = '\0';
    else *p = '\0';
}

/// Joined into @p out; false when it does not fit.
bool join(char* out, const char* a, const char* b, const char* c = nullptr) {
    const size_t na = std::strlen(a), nb = std::strlen(b), nc = c ? std::strlen(c) : 0;
    const size_t n = na + 1 + nb + (c ? 1 + nc : 0);
    if (n >= kPath) return false;
    std::memmove(out, a, na);
    out[na] = '/';
    std::memcpy(out + na + 1, b, nb);
    if (c) {
        out[na + 1 + nb] = '/';
        std::memcpy(out + na + 2 + nb, c, nc);
    }
    out[n] = '\0';
    return true;
}

/// FAT compares names without case: so does every path match here.
bool sameName(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
            return false;
    }
    return *a == *b;
}

/// "a/b/../c/./d" -> "a/c/d", in place. A leading '/' is kept; ".." above
/// the top is dropped.
void normalize(char* p) {
    const bool abs = *p == '/';
    char* top = p + (abs ? 1 : 0);
    char* out = top;
    const char* in = top;
    while (*in) {
        while (*in == '/') ++in;
        const char* seg = in;
        while (*in && *in != '/') ++in;
        const size_t n = static_cast<size_t>(in - seg);
        if (n == 0 || (n == 1 && seg[0] == '.')) continue;
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            while (out > top && out[-1] != '/') --out;
            if (out > top) --out;
            continue;
        }
        if (out > top) *out++ = '/';
        std::memmove(out, seg, n);
        out += n;
    }
    *out = '\0';
}

/// @p path is below @p dir: both normalised.
bool isUnder(const char* path, const char* dir) {
    const size_t n = std::strlen(dir);
    return std::strncmp(path, dir, n) == 0 && path[n] == '/' && path[n + 1];
}

/// A sample of a kit as a card path, every "." and ".." resolved: the only
/// form any decision below is made on.
bool samplePath(const std::string& kitJson, const std::string& sample, char* out) {
    const std::string fs = kitFsPath(resolveKitSamplePath(kitJson, sample));
    if (fs.empty() || fs.size() >= kPath) return false;
    std::memcpy(out, fs.c_str(), fs.size() + 1);
    normalize(out);
    return *baseOf(out) != '\0';
}

bool hasWavSuffix(const char* p) {
    const size_t n = std::strlen(p);
    return n > 4 && sameName(p + n - 4, ".wav");
}

/// mkdir -p, but never at or above @p floor.
bool makeDirs(IKitConvertHost& host, const char* path, const char* floor) {
    if (std::strlen(path) <= std::strlen(floor) || isDirPath(path)) return true;
    char up[kPath];
    std::snprintf(up, sizeof(up), "%s", path);
    cutLast(up);
    if (!makeDirs(host, up, floor)) return false;
    return host.makeDir(path) || isDirPath(path);
}

/// The kit as the model reads it -- the same parse every kit load does.
bool readKit(IKitConvertHost& host, const char* fsPath, KitInfo& kit) {
    FILE* f = std::fopen(fsPath, "r");
    // What a save cut between its two renames leaves.
    if (!f && errno == ENOENT) {
        char bak[kPath];
        std::snprintf(bak, sizeof(bak), "%s.bak", fsPath);
        f = std::fopen(bak, "r");
    }
    if (!f) return false;
    // A kit is a few kB; a document of megabytes is not one, and parsing it
    // would take the PSRAM the UI lives in.
    const bool sane = std::fseek(f, 0, SEEK_END) == 0 && std::ftell(f) >= 0 &&
                      std::ftell(f) <= kMaxJsonBytes && std::fseek(f, 0, SEEK_SET) == 0;
    bool ok = false;
    if (sane) {
        JsonDocument doc(host.jsonAllocator() ? host.jsonAllocator()
                                              : ArduinoJson::detail::DefaultAllocator::instance());
        JsonFileReader in(f);
        ok = !deserializeJson(doc, in) && kit_parse_metadata(kit, doc);
        if (ok) kit_parse_pads(kit, doc);
    }
    std::fclose(f);
    return ok;
}

/// Through `.tmp` and `.bak`, as PortableKitLoader saves a kit: a failed
/// write costs the write, never the file already there.
bool replaceFile(const char* tmp, const char* path) {
    char bak[kPath];
    std::snprintf(bak, sizeof(bak), "%s.bak", path);
    bool hadOld = false;
    if (pathExists(path)) {
        std::remove(bak);
        hadOld = std::rename(path, bak) == 0;
    }
    if (std::rename(tmp, path) != 0) {
        if (hadOld) std::rename(bak, path);
        std::remove(tmp);
        return false;
    }
    std::remove(bak);
    return true;
}

bool writeKit(IKitConvertHost& host, const char* fsPath, const KitInfo& kit) {
    char tmp[kPath];
    std::snprintf(tmp, sizeof(tmp), "%s.tmp", fsPath);
    FILE* f = std::fopen(tmp, "w");
    if (!f) return false;
    {
        JsonDocument doc(host.jsonAllocator() ? host.jsonAllocator()
                                              : ArduinoJson::detail::DefaultAllocator::instance());
        kit_serialize(kit, doc);
        JsonFileWriter out{f};
        serializeJson(doc, out);
    }
    if (!closeWritten(f)) { std::remove(tmp); return false; }
    return replaceFile(tmp, fsPath);
}

/// A small text file whole (the tidy's list); empty when it is not there or
/// is larger than any list this writes.
std::string readText(const char* path, long cap) {
    std::string out;
    FILE* f = std::fopen(path, "r");
    if (!f) return out;
    if (std::fseek(f, 0, SEEK_END) == 0) {
        const long n = std::ftell(f);
        if (n > 0 && n <= cap && std::fseek(f, 0, SEEK_SET) == 0) {
            out.resize(static_cast<size_t>(n));
            out.resize(std::fread(&out[0], 1, out.size(), f));
        }
    }
    std::fclose(f);
    return out;
}

/// The list replaced whole, or removed when it is empty.
bool writeText(const char* path, const std::string& text) {
    if (text.empty()) {
        std::remove(path);
        return !pathExists(path);
    }
    char tmp[kPath];
    std::snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = std::fopen(tmp, "w");
    if (!f) return false;
    const bool wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (!closeWritten(f) || !wrote) { std::remove(tmp); return false; }
    std::remove(path);
    return std::rename(tmp, path) == 0;
}

/// Whether @p line is one of the '\n'-ended lines of @p text, as FAT
/// compares names.
bool hasLine(const std::string& text, const char* line) {
    const size_t n = std::strlen(line);
    for (size_t at = 0; at < text.size();) {
        size_t e = text.find('\n', at);
        if (e == std::string::npos) e = text.size();
        size_t k = 0;
        while (k < n && e - at == n && std::tolower(static_cast<unsigned char>(text[at + k])) ==
                                           std::tolower(static_cast<unsigned char>(line[k]))) ++k;
        if (e - at == n && k == n) return true;
        at = e + 1;
    }
    return false;
}

void removeEmptyDir(const char* path) {
#ifdef _WIN32
    std::error_code ec;
    if (std::filesystem::is_empty(std::filesystem::u8path(path), ec)) {
        std::filesystem::remove(std::filesystem::u8path(path), ec);
    }
#else
    rmdir(path);   // fails, harmlessly, while anything is in it
#endif
}

/// @p needle in @p hay, ignoring ASCII case.
bool containsCi(const char* hay, size_t n, const char* needle) {
    const size_t m = std::strlen(needle);
    for (size_t i = 0; m && i + m <= n; ++i) {
        size_t k = 0;
        while (k < m && std::tolower(static_cast<unsigned char>(hay[i + k])) ==
                            std::tolower(static_cast<unsigned char>(needle[k]))) ++k;
        if (k == m) return true;
    }
    return false;
}

/// Every kit JSON under @p dir (a buffer of kPath, extended and restored as
/// the walk goes), found as the kit scan finds them: a folder with kit.json
/// or <folder>.json is a kit and is not looked into further.
template <typename Fn>
void forEachKitJson(char* dir, Fn& fn, int depth = 0) {
#ifdef _WIN32
    if (depth > 8) return;
    std::error_code ec;
    const std::string here = dir;
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::u8path(here), ec)) {
        const std::string name = e.path().filename().u8string();
        if (name.empty() || name[0] == '.' || !e.is_directory(ec)) continue;
        const std::string sub = here + "/" + name;
        if (sub.size() + name.size() + 7 >= kPath) continue;
        std::string json = sub + "/kit.json";
        if (!pathExists(json.c_str())) json = sub + "/" + name + ".json";
        if (pathExists(json.c_str())) {
            fn(json.c_str());
        } else {
            std::snprintf(dir, kPath, "%s", sub.c_str());
            forEachKitJson(dir, fn, depth + 1);
            std::snprintf(dir, kPath, "%s", here.c_str());
        }
    }
#else
    if (depth > 8) return;
    DIR* d = opendir(dir);
    if (!d) return;
    const size_t len = std::strlen(dir);
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        if (len + 2 + 2 * std::strlen(e->d_name) + 6 >= kPath) continue;
        std::snprintf(dir + len, kPath - len, "/%s", e->d_name);
        if (isDirPath(dir)) {
            char json[kPath];
            const size_t sub = std::strlen(dir);
            join(json, dir, "kit.json");
            if (!pathExists(json)) {
                join(json, dir, e->d_name);
                std::strcat(json, ".json");      // room checked above
            }
            if (pathExists(json)) fn(json);
            else forEachKitJson(dir, fn, depth + 1);
            dir[sub] = '\0';
        }
        dir[len] = '\0';
    }
    closedir(d);
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

const char* rateFolderName(uint32_t rate, char* buf) {
    std::snprintf(buf, kRateFolderMax, "%uk%03u", static_cast<unsigned>(rate / 1000 % 10000),
                  static_cast<unsigned>(rate % 1000));
    char* end = buf + std::strlen(buf);
    while (end[-1] == '0' && end[-2] != 'k') *--end = '\0';
    if (end[-1] == '0' && end[-2] == 'k') end[-1] = '\0';
    return buf;
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
            if (chunkCount < kMaxChunks) chunks[chunkCount++] = c;   // more are not copied
        }
        if (size > avail) break;                // runs past the file: nothing after it
        pos += 8 + size + (size & 1);           // chunks are word aligned
    }
    if (!haveFmt || !haveData) return false;
    const uint32_t frameBytes = static_cast<uint32_t>(channels) * (bits / 8);
    frames = frameBytes ? dataBytes / frameBytes : 0;
    return true;
}

static bool wavChunkScaled(const char id[4]) {
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

uint64_t IKitConvertHost::freeBytes(const char* fsDir) {
    (void)fsDir;
    return UINT64_MAX;
}

bool IKitConvertHost::makeDir(const char* fsPath) {
#ifdef _WIN32
    std::error_code ec;
    return std::filesystem::create_directory(std::filesystem::u8path(fsPath), ec);
#else
    return mkdir(fsPath, 0755) == 0;
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
    char part[kPath] = {};
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

uint32_t KitRateConverter::addName(const char* s) {
    const uint32_t off = static_cast<uint32_t>(names_.size());
    names_.append(s, std::strlen(s) + 1);
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
    target_ = targetRate;
    names_.clear();
    files_.clear();
    edits_.clear();
    cursor_ = 0;
    kitRead_ = false;
    progress_ = Progress();
    progress_.toRate = targetRate;
    cancel_.store(false, std::memory_order_relaxed);
    const std::string fs = kitFsPath(kitJsonPath);
    if (fs.empty() || fs.size() >= kPath || targetRate == 0) return fail("no kit");
    std::memcpy(kitDirFs_, fs.c_str(), fs.size() + 1);
    cutLast(kitDirFs_);
    normalize(kitDirFs_);
    if (!kitDirFs_[0]) return fail("no kit");
    progress_.phase = Phase::Planning;
    return true;
}

bool KitRateConverter::planKit() {
    KitInfo* kit = new (std::nothrow) KitInfo();
    if (!kit) return fail("not enough memory");
    const bool read = readKit(*host_, kitFsPath(kitJson_).c_str(), *kit);
    if (!read || kit->engine == KitEngine::Pitched) {
        delete kit;
        return fail(read ? "pitched kit" : "cannot read the kit");
    }
    char folderFs[kPath], src[kPath], rel[kPath], dst[kPath];
    char folderBuf[kRateFolderMax];
    const char* folder = rateFolderName(target_, folderBuf);
    join(folderFs, kitDirFs_, folder);
    for (const Pad& pad : kit->pads) {
        for (const SampleSlot& slot : pad.samples) {
            // Resolved, then every ".." taken out, before anything is decided
            // on it: "../../x.wav" begins with the kit's folder as text and is
            // not in it.
            if (!samplePath(kitJson_, slot.sample, src)) continue;
            bool known = false;
            for (const FileJob& f : files_) known = known || sameName(name(f.src), src);
            if (known) continue;
            FileJob f;
            f.inside = isUnder(src, kitDirFs_);
            if (f.inside) {
                std::snprintf(rel, sizeof(rel), "%s/%s", folder, src + std::strlen(kitDirFs_) + 1);
            } else {
                // Borrowed from elsewhere on the card: copied in, never
                // touched. A name already taken gets a number.
                const char* base = baseOf(src);
                const char* dot = std::strrchr(base, '.');
                const int stem = dot ? static_cast<int>(dot - base) : static_cast<int>(std::strlen(base));
                for (int n = 1; n < 100; ++n) {
                    if (n == 1) std::snprintf(rel, sizeof(rel), "%s/_shared/%s", folder, base);
                    else std::snprintf(rel, sizeof(rel), "%s/_shared/%.*s_%d%s", folder, stem, base, n,
                                       dot ? dot : "");
                    bool taken = false;
                    for (const FileJob& g : files_) taken = taken || sameName(name(g.rel), rel);
                    if (!taken) break;
                }
            }
            // Whatever the names were, the file written is in the rate's folder.
            if (!join(dst, kitDirFs_, rel)) continue;
            normalize(dst);
            if (!isUnder(dst, folderFs)) continue;
            f.src = addName(src);
            f.rel = addName(rel);
            f.dst = addName(dst);
            files_.push_back(f);
        }
    }
    delete kit;
    progress_.files = static_cast<uint16_t>(files_.size());
    return true;
}

bool KitRateConverter::planFile(FileJob& f) {
    FILE* in = std::fopen(name(f.src), "rb");
    WavLayout lay;
    const bool ok = in && lay.parse(in);
    if (in) std::fclose(in);
    if (ok && lay.pcm16() && lay.rate == target_) {
        f.state = FileState::Skip;   // already at the rate: nothing to do, nothing wrong
        f.rate = lay.rate;
        return true;
    }
    // Converted, it has to fit every 32-bit size a WAV has, with room for its
    // metadata (an 8 kHz file of 900 MB would be 5 GB at 44.1 kHz); and the
    // store keys a sample on its path, so one too long for it, ".part" and
    // all, is a pad that would never load.
    const uint64_t outFrames = ok ? (static_cast<uint64_t>(lay.frames) * target_ + lay.rate / 2) / lay.rate : 0;
    if (!ok || !lay.pcm16() || outFrames * lay.channels * 2 > kMaxOutBytes ||
        std::strlen(name(f.dst)) + 6 >= SampleStore::kMaxPath) {
        f.state = FileState::Skip;
        ++progress_.skipped;
        return true;
    }
    f.rate = lay.rate;
    f.frames = lay.frames;
    f.channels = static_cast<uint8_t>(lay.channels);
    f.srcBytes = lay.fileBytes;
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
    uint32_t bestCount = 0;
    for (const FileJob& a : files_) {
        if (!a.rate || a.rate == target_) continue;
        uint32_t n = 0;
        for (const FileJob& b : files_) n += b.rate == a.rate;
        if (n > bestCount) { progress_.fromRate = a.rate; bestCount = n; }
    }
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
    char part[kPath];
    std::memcpy(part, io_->part, sizeof(part));
    endFileIo();
    if (part[0]) std::remove(part);
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
    const bool scaled = wavChunkScaled(c.id);
    if (wavChunkDropped(c.id) || (scaled && c.size > kMaxScaledChunk)) return true;
    /* A chunk that carries positions is read whole, scaled, then written; any
     * other goes through the piece buffer as it is. */
    uint8_t* buf = scaled ? static_cast<uint8_t*>(host.alloc(c.size ? c.size : 1)) : piece;
    if (!buf) return false;
    bool ok = std::fseek(src, c.offset, SEEK_SET) == 0;
    if (scaled) {
        for (uint32_t at = 0; ok && at < c.size; at += step) {
            const uint32_t n = std::min(step, c.size - at);
            ok = host.readIo(src, buf + at, n) == n;
        }
        ok = ok && scaleWavChunk(c.id, buf, c.size, inRate, outRate);
    }
    ok = ok && host.writeIo(dst, hdr, 8);
    for (uint32_t at = 0; ok && at < c.size; at += step) {
        const uint32_t n = std::min(step, c.size - at);
        if (!scaled) ok = host.readIo(src, piece, n) == n;
        ok = ok && host.writeIo(dst, scaled ? buf + at : piece, n);
        host.giveWay();
    }
    if (ok && (c.size & 1)) ok = host.writeIo(dst, &pad, 1);
    if (scaled) host.release(buf);
    return ok;
}

bool KitRateConverter::copyChunks(bool afterData) {
    Io& io = *io_;
    const FileJob& f = files_[cursor_];
    for (uint8_t i = 0; i < io.lay.chunkCount; ++i) {
        if (io.lay.chunks[i].afterData != afterData) continue;
        if (!copyChunk(*host_, io.src, io.dst, io.lay.chunks[i], f.rate, target_,
                       reinterpret_cast<uint8_t*>(io.out), io.outCap * f.channels * 2))
            return false;
    }
    return true;
}

bool KitRateConverter::startFile() {
    FileJob& f = files_[cursor_];
    ++progress_.fileIndex;
    setText(progress_.current, sizeof(progress_.current), baseOf(name(f.src)));

    char dir[kPath];
    std::snprintf(dir, sizeof(dir), "%s", name(f.dst));
    cutLast(dir);
    if (!makeDirs(*host_, dir, kitDirFs_)) return fail("cannot make the folder");

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
    /* What one push can hand back: its own frames at the ratio, plus up to two
     * analysis blocks the flush finishes, plus the stream's slack. */
    const uint32_t block = kConvertFftMax / kConvertOversample;
    io.outCap = static_cast<uint32_t>(static_cast<uint64_t>(kChunkFrames + 2 * block) * target_ / f.rate) +
                2 * FftResamplerStream::kStreamOutSlack + 1;
    const size_t samples = (static_cast<size_t>(kChunkFrames) * 2 + static_cast<size_t>(io.outCap) * 2) * ch;
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

    std::snprintf(io.part, sizeof(io.part), "%s.part", name(f.dst));
    std::remove(io.part);
    io.dst = std::fopen(io.part, "wb");
    if (!io.dst) { io.part[0] = '\0'; return fail("cannot write to the card"); }

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
    if (!host_->writeIo(io.dst, h, sizeof(h)) || !copyChunks(false)) return fail("cannot write to the card");
    uint8_t d[8];
    std::memcpy(d, "data", 4);
    wr32(d + 4, io.outTotal * ch * 2);
    const long at = std::ftell(io.dst);
    if (at < 0 || !host_->writeIo(io.dst, d, 8)) return fail("cannot write to the card");
    io.dataSizeAt = static_cast<uint32_t>(at) + 4;
    if (std::fseek(io.src, io.lay.dataOffset, SEEK_SET) != 0) return fail("cannot read a sample");
    return true;
}

/// @p got frames of each channel in io.chOut, interleaved and written --
/// never past the length the file is to have.
bool KitRateConverter::emit(uint32_t got) {
    Io& io = *io_;
    const uint32_t ch = files_[cursor_].channels;
    if (io.outWritten + got > io.outTotal) got = io.outTotal - io.outWritten;
    for (uint32_t i = 0; i < got; ++i)
        for (uint32_t c = 0; c < ch; ++c) io.out[i * ch + c] = io.chOut[c][i];
    const uint8_t* b = reinterpret_cast<const uint8_t*>(io.out);
    const uint32_t bytes = got * ch * 2;
    for (uint32_t at = 0; at < bytes; at += kConvertPieceBytes) {
        if (!host_->writeIo(io.dst, b + at, std::min<uint32_t>(kConvertPieceBytes, bytes - at))) return false;
    }
    io.outWritten += got;
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
    if (!emit(got)) return fail("cannot write to the card");
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
    bool ok = emit(got);
    // A stream that came up short: the rest is the silence past the end.
    while (ok && io.outWritten < io.outTotal) {
        const uint32_t n = std::min(io.outCap, io.outTotal - io.outWritten);
        for (uint32_t c = 0; c < ch; ++c) std::memset(io.chOut[c], 0, n * sizeof(int16_t));
        ok = emit(n);
    }
    ok = ok && copyChunks(true);
    const long end = ok ? std::ftell(io.dst) : -1;
    uint8_t v[4];
    wr32(v, static_cast<uint32_t>(end) - 8);
    ok = end > 0 && std::fseek(io.dst, 4, SEEK_SET) == 0 && host_->writeIo(io.dst, v, 4);
    wr32(v, io.outTotal * ch * 2);
    ok = ok && std::fseek(io.dst, io.dataSizeAt, SEEK_SET) == 0 && host_->writeIo(io.dst, v, 4);
    ok = ok && host_->syncFile(io.dst);
    ok = std::fclose(io.dst) == 0 && ok;
    io.dst = nullptr;
    if (!ok) return fail("cannot write to the card");

    // Read back before it counts: a file the card did not keep is not swapped in.
    FILE* chk = std::fopen(io.part, "rb");
    WavLayout back;
    const bool parsed = chk && back.parse(chk);
    if (chk) std::fclose(chk);
    if (!parsed || !back.pcm16() || back.rate != target_ || back.channels != ch ||
        back.frames != io.outTotal || back.riffBytes != back.fileBytes)
        return fail("verify failed");
    std::remove(name(f.dst));                    // a stale one that did not verify
    if (std::rename(io.part, name(f.dst)) != 0) return fail("cannot write to the card");
    io.part[0] = '\0';
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

    /* The kit as the card has it now -- edits saved during the conversion
     * included -- read, retargeted and saved the way every kit save is. */
    const std::string jsonFs = kitFsPath(kitJson_);
    KitInfo* kit = new (std::nothrow) KitInfo();
    if (!kit) return fail("not enough memory");
    if (!readKit(*host_, jsonFs.c_str(), *kit)) { delete kit; return fail("cannot read the kit"); }
    char src[kPath];
    for (uint8_t p = 0; p < KIT_PADS; ++p) {
        for (SampleSlot& slot : kit->pads[p].samples) {
            if (!samplePath(kitJson_, slot.sample, src)) continue;
            for (const FileJob& f : files_) {
                if (f.state != FileState::Done || !sameName(name(f.src), src)) continue;
                KitConvertEdit e;
                e.pad = p;
                e.oldSample = slot.sample;
                e.oldStart = slot.start;
                e.oldEnd = slot.end;
                e.newSample = name(f.rel);
                e.start = scaleFrame(slot.start, f.rate, target_);
                e.end = scaleFrame(slot.end, f.rate, target_);
                slot.sample = e.newSample;
                slot.start = e.start;
                slot.end = e.end;
                edits_.push_back(std::move(e));
                break;
            }
        }
    }
    if (edits_.empty()) { delete kit; progress_.phase = Phase::Done; return false; }

    // What the tidy is to remove, written before the kit stops naming it: a
    // cut between the two leaves a list whose files the kit still names, and
    // the tidy keeps those.
    char folderFs[kPath], journal[kPath];
    char folder[kRateFolderMax];
    join(folderFs, kitDirFs_, rateFolderName(target_, folder));
    join(journal, folderFs, kJournal);
    std::string list = readText(journal, kMaxListBytes);
    for (const FileJob& f : files_) {
        if (f.state != FileState::Done || !f.inside) continue;
        const char* rel = name(f.src) + std::strlen(kitDirFs_) + 1;
        if (hasLine(list, rel)) continue;
        list += rel;
        list += '\n';
    }
    const bool saved = makeDirs(*host_, folderFs, kitDirFs_) && writeText(journal, list) &&
                       writeKit(*host_, jsonFs.c_str(), *kit);
    delete kit;
    if (!saved) {
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
    const bool stop = cancel_.load(std::memory_order_relaxed);
    switch (progress_.phase) {
    case Phase::Planning:
        if (stop) break;
        if (!kitRead_) {
            kitRead_ = true;
            if (!planKit()) return false;
            if (files_.empty()) { progress_.phase = Phase::Done; return false; }
            return true;
        }
        if (cursor_ < files_.size()) { planFile(files_[cursor_++]); return true; }
        return planDone();

    case Phase::Converting:
        if (stop) { dropFile(); break; }
        if (!io_) {
            while (cursor_ < files_.size() && files_[cursor_].state != FileState::Pending) ++cursor_;
            if (cursor_ >= files_.size()) { progress_.phase = Phase::Committing; return true; }
            if (!startFile()) return false;
            if (!io_) ++cursor_;      // skipped: changed since the plan
            return true;
        }
        return stepFile() || progress_.phase != Phase::Failed;

    case Phase::Committing:
        if (stop) break;
        return commit();

    default:
        return false;
    }
    progress_.phase = Phase::Cancelled;
    return false;
}

// ── After the commit ────────────────────────────────────────────────────────

bool kitTidyPending(const std::string& kitJsonPath, uint32_t targetRate) {
    char dir[kPath], path[kPath];
    std::snprintf(dir, sizeof(dir), "%s", kitFsPath(kitJsonPath).c_str());
    cutLast(dir);
    char folder[kRateFolderMax];
    return join(path, dir, rateFolderName(targetRate, folder), kJournal) && pathExists(path);
}

KitTidyResult tidyKit(IKitConvertHost& host, const std::string& kitJsonPath,
                      const std::string& kitsRoot, uint32_t targetRate) {
    KitTidyResult r;
    const std::string jsonFs = kitFsPath(kitJsonPath);
    char kitDir[kPath], folderFs[kPath], journal[kPath], path[kPath];
    if (jsonFs.size() >= kPath) return r;
    std::memcpy(kitDir, jsonFs.c_str(), jsonFs.size() + 1);
    cutLast(kitDir);
    normalize(kitDir);
    char folder[kRateFolderMax];
    join(folderFs, kitDir, rateFolderName(targetRate, folder));
    join(journal, folderFs, kJournal);
    const std::string list = readText(journal, kMaxListBytes);
    if (list.empty()) { r.ok = !pathExists(journal) || writeText(journal, list); return r; }

    KitInfo* kit = new (std::nothrow) KitInfo();
    if (!kit) return r;
    if (!readKit(host, jsonFs.c_str(), *kit)) { delete kit; return r; }   // not ok: nothing removed

    /* Another kit can reach into this one's folder only by an absolute path
     * or one with "..", and either carries "/<folder>/". Those kits are read
     * as text, and a file of this one they might name -- "/<folder>/" and its
     * path in the kit, or its bare name after it -- is kept. No JSON, no path
     * resolution per sample. */
    const char* folderName = baseOf(kitDir);
    char needle[kPath];
    std::snprintf(needle, sizeof(needle), "/%s/", folderName);
    std::string others;          // the texts of the kits that do
    char walk[kPath];
    std::snprintf(walk, sizeof(walk), "%s", kitFsPath(kitsRoot).c_str());
    normalize(walk);
    auto scan = [&](const char* other) {
        if (sameName(other, jsonFs.c_str())) return;
        const std::string text = readText(other, kMaxScanBytes);
        if (containsCi(text.data(), text.size(), needle)) others += text;
        host.giveWay();
    };
    forEachKitJson(walk, scan);

    /* The list is a file on the card, and a card is anyone's to write: a line
     * is an original only if it names a WAV in the kit's own folder -- not
     * above it, not the kit's JSON, not the converted files themselves. Any
     * other line is dropped from the list and nothing is done with it. */
    // What this kit's own JSON names, resolved once.
    std::string named;
    char src[kPath];
    for (const Pad& pad : kit->pads) {
        for (const SampleSlot& slot : pad.samples) {
            if (!samplePath(kitJsonPath, slot.sample, src)) continue;
            named += src;
            named += '\n';
        }
    }
    delete kit;

    std::string left;
    for (size_t at = 0; at < list.size();) {
        size_t e = list.find('\n', at);
        if (e == std::string::npos) e = list.size();
        const std::string line = list.substr(at, e - at);
        at = e + 1;
        if (line.empty()) continue;
        bool keep = line[0] == '/' || !join(path, kitDir, line.c_str());
        if (!keep) {
            normalize(path);
            keep = !isUnder(path, kitDir) || isUnder(path, folderFs) || !hasWavSuffix(path) || hasLine(named, path);
            if (!keep && !others.empty()) {
                char ref[kPath];
                std::snprintf(ref, sizeof(ref), "%s%s", needle, path + std::strlen(kitDir) + 1);
                keep = containsCi(others.data(), others.size(), ref);
                std::snprintf(ref, sizeof(ref), "%s%s", needle, baseOf(path));
                keep = keep || containsCi(others.data(), others.size(), ref);
            }
        }
        if (keep) { ++r.kept; continue; }
        if (host.releaseFile(path) && (std::remove(path) == 0 || !pathExists(path))) {
            ++r.removed;
            cutLast(path);
            if (std::strlen(path) > std::strlen(kitDir)) removeEmptyDir(path);
        } else {
            ++r.pending;
            left += line;
            left += '\n';
        }
    }
    r.ok = writeText(journal, left);
    return r;
}

} // namespace crosspad

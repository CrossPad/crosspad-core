// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "crosspad/fw/FwDownloader.hpp"

using namespace crosspad;

namespace {

using Bytes = std::vector<uint8_t>;

constexpr const char* kBase = "http://test:8765";
constexpr const char* kRoot = "/sd/crosspad/firmware";
constexpr const char* kPkg = "/sd/crosspad/firmware/full-1.3.0";
constexpr uint8_t kRev = 2;
const std::string kGoodSig = "signed";

const FwPackageLayout kLayout{"/sd/crosspad", kRoot, ".complete", ".downloading", ".pinned", ".part"};

// Not SHA-256: four FNV-1a lanes are enough to tell files apart, and streaming
// them proves that a resumed file is hashed whole.
struct Fnv {
    uint64_t lane[4] = {1469598103934665603ull, 1099511628211ull, 0x9e3779b97f4a7c15ull, 0x2545f4914f6cdd1dull};
    void update(const uint8_t* d, size_t n) {
        for (size_t i = 0; i < n; ++i)
            for (int l = 0; l < 4; ++l) lane[l] = (lane[l] ^ d[i]) * 1099511628211ull;
    }
    void digest(uint8_t out[ISha256::kDigestLen]) const {
        for (int l = 0; l < 4; ++l)
            for (int b = 0; b < 8; ++b) out[l * 8 + b] = static_cast<uint8_t>(lane[l] >> (8 * b));
    }
};

std::string hashHex(const Bytes& data) {
    Fnv f;
    f.update(data.data(), data.size());
    uint8_t d[ISha256::kDigestLen];
    f.digest(d);
    char hex[2 * ISha256::kDigestLen + 1];
    for (size_t i = 0; i < ISha256::kDigestLen; ++i) std::snprintf(hex + 2 * i, 3, "%02x", d[i]);
    return hex;
}

struct FakeSha : ISha256 {
    Fnv f;
    int starts = 0;
    void start() override {
        f = Fnv{};
        ++starts;
    }
    void update(const uint8_t* data, size_t n) override { f.update(data, n); }
    void finish(uint8_t digest[kDigestLen]) override { f.digest(digest); }
};

Bytes pattern(size_t n, uint8_t seed) {
    Bytes b(n);
    for (size_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>(seed + i * 7 + (i >> 8));
    return b;
}

std::string parentOf(const std::string& path) { return path.substr(0, path.rfind('/')); }

struct FakeFiles : IFwFiles {
    struct Handle {
        std::string path;
        size_t pos;
    };
    std::map<std::string, Bytes> files;
    std::set<std::string> dirs{"/sd", "/sd/crosspad", kRoot};
    std::vector<std::string> ops;     // every change, in order
    std::set<std::string> unreadable; // open for reading fails
    std::string failWritePath;        // a write to it fails
    size_t openCount = 0;

    void put(const std::string& path, const Bytes& data) {
        dirs.insert(parentOf(path));
        files[path] = data;
    }
    void put(const std::string& path, const std::string& text) { put(path, Bytes(text.begin(), text.end())); }
    bool has(const std::string& path) const { return files.count(path) != 0; }
    std::string text(const std::string& path) const {
        const Bytes& b = files.at(path);
        return std::string(b.begin(), b.end());
    }
    std::vector<std::string> under(const std::string& dir) const {
        std::vector<std::string> out;
        for (const auto& kv : files)
            if (kv.first.compare(0, dir.size() + 1, dir + "/") == 0) out.push_back(kv.first.substr(dir.size() + 1));
        return out;
    }

    void* open(const char* path, FwFileMode mode) override {
        if (mode == FwFileMode::Read) {
            if (!has(path) || unreadable.count(path)) return nullptr;
        } else {
            if (!dirs.count(parentOf(path))) return nullptr;
            if (mode == FwFileMode::Write || !has(path)) files[path].clear();
            ops.push_back(std::string(mode == FwFileMode::Write ? "write " : "append ") + path);
        }
        ++openCount;
        return new Handle{path, mode == FwFileMode::Append ? files[path].size() : 0};
    }
    size_t read(void* f, void* buf, size_t n) override {
        auto* h = static_cast<Handle*>(f);
        const Bytes& b = files[h->path];
        const size_t take = std::min(n, b.size() - h->pos);
        std::memcpy(buf, b.data() + h->pos, take);
        h->pos += take;
        return take;
    }
    size_t write(void* f, const void* data, size_t n) override {
        auto* h = static_cast<Handle*>(f);
        if (h->path == failWritePath) return 0;
        Bytes& b = files[h->path];
        const auto* p = static_cast<const uint8_t*>(data);
        b.insert(b.end(), p, p + n);
        h->pos += n;
        return n;
    }
    bool close(void* f) override {
        delete static_cast<Handle*>(f);
        --openCount;
        return true;
    }
    uint64_t size(const char* path) override { return has(path) ? files[path].size() : 0; }
    bool exists(const char* path) override { return has(path); }
    bool makeDir(const char* path) override {
        if (!dirs.count(path)) ops.push_back(std::string("mkdir ") + path);
        dirs.insert(path);
        return true;
    }
    bool remove(const char* path) override {
        if (!has(path)) return false;
        files.erase(path);
        ops.push_back(std::string("remove ") + path);
        return true;
    }
    bool rename(const char* from, const char* to) override {
        if (!has(from) || has(to)) return false;
        files[to] = files[from];
        files.erase(from);
        ops.push_back(std::string("rename ") + from + " " + to);
        return true;
    }
};

// Answers like the local test server and the Worker: 200, 206 with the asked range, 416.
struct FakeHttp : IHttpStream {
    struct Request {
        std::string url;
        uint64_t offset;
    };
    std::map<std::string, Bytes> bodies;
    std::map<std::string, int> forcedStatus;  // answered instead of the body, every time
    bool ignoreRange = false;                 // a resume answered 200 with the whole file
    bool otherRange = false;                  // a resume answered 206 for another range
    int status416Once = 0;                    // this many resumes answered 416
    size_t dropAt = 0;                        // the stream ends at this byte of the file ...
    int drops = 0;                            // ... this many times
    std::vector<Request> requests;

    void serve(const std::string& name, const Bytes& body) { bodies[url(name)] = body; }
    void serve(const std::string& name, const std::string& body) { serve(name, Bytes(body.begin(), body.end())); }
    static std::string url(const std::string& name) { return std::string(kBase) + "/api/device/file/v1.3.0/" + name; }
    size_t streamed(const std::string& name) const {
        size_t n = 0;
        for (const auto& r : requests) n += r.url == url(name);
        return n;
    }

    bool getBuffer(const char* u, char* buf, size_t cap, size_t* len, HttpStreamReply* out) override {
        *out = {};
        *len = 0;
        requests.push_back({u, 0});
        if (forcedStatus.count(u)) {
            out->status = forcedStatus[u];
            return false;
        }
        auto it = bodies.find(u);
        if (it == bodies.end()) return false;   // no response
        out->status = 200;
        if (it->second.size() + 1 > cap) return false;
        std::memcpy(buf, it->second.data(), it->second.size());
        *len = it->second.size();
        buf[*len] = '\0';
        return true;
    }

    bool getStream(const char* u, uint64_t offset, uint64_t expectedTotal, Sink sink, void* ctx, uint8_t* chunk,
                   size_t chunkLen, HttpStreamReply* out) override {
        *out = {};
        requests.push_back({u, offset});
        if (forcedStatus.count(u)) {
            out->status = forcedStatus[u];
            return false;
        }
        auto it = bodies.find(u);
        if (it == bodies.end()) {
            out->status = 404;
            return false;
        }
        const Bytes& body = it->second;
        if (offset > 0) {
            if (ignoreRange) {
                out->status = 200;
                return false;
            }
            if (status416Once > 0 || offset >= body.size()) {
                --status416Once;
                out->status = 416;
                return false;
            }
            out->status = 206;
            if (otherRange || (expectedTotal > 0 && expectedTotal != body.size())) {
                out->rangeMismatch = true;
                return false;
            }
        } else {
            out->status = 200;
        }
        size_t end = body.size();
        const bool drop = drops > 0 && dropAt >= offset && dropAt < end;
        if (drop) {
            end = dropAt;
            --drops;
        }
        for (size_t pos = offset; pos < end;) {
            const size_t n = std::min(chunkLen, end - pos);
            std::memcpy(chunk, body.data() + pos, n);
            if (!sink(chunk, n, ctx)) return false;
            pos += n;
        }
        return !drop;
    }
};

struct FakePlatform : IFwDownloadPlatform {
    FakeFiles& files;
    FwBlocklist blocklist;
    FwDlResult room = FwDlResult::Ok;
    std::vector<uint64_t> cleanups;
    std::vector<uint32_t> sleeps;
    std::vector<FwDlEvent> events;
    std::vector<std::string> removed;
    bool outOfMemory = false;
    int yields = 0;
    int allocs = 0;

    explicit FakePlatform(FakeFiles& f) : files(f) {}

    void* allocBuffers(size_t bytes) override {
        if (outOfMemory) return nullptr;
        ++allocs;
        return ::operator new(bytes);
    }
    void freeBuffers(void* p) override {
        --allocs;
        ::operator delete(p);
    }
    bool verifyManifest(const uint8_t*, size_t, const uint8_t* sig, size_t sigLen) override {
        return std::string(reinterpret_cast<const char*>(sig), sigLen) == kGoodSig;
    }
    uint8_t boardRev() override { return kRev; }
    uint64_t packageBytes(const char* name) override {
        uint64_t sum = 0;
        const std::string dir = std::string(kRoot) + "/" + name;
        for (const std::string& f : files.under(dir)) sum += files.files[dir + "/" + f].size();
        return sum;
    }
    void removePackage(const char* name) override {
        removed.push_back(name);
        const std::string dir = std::string(kRoot) + "/" + name;
        for (const std::string& f : files.under(dir)) files.files.erase(dir + "/" + f);
        files.dirs.erase(dir);
    }
    FwDlResult cleanup(uint64_t incoming, const char*, const char*, const char*) override {
        cleanups.push_back(incoming);
        return room;
    }
    bool isBlocked(const char* kit, const char* version) override { return blocklist.has(kit, version); }
    void block(const char* kit, const char* version) override {
        FwBlockedVersion forgotten;
        bool dropped;
        blocklist.add(kit, version, &forgotten, &dropped);
    }
    void yieldIo() override { ++yields; }
    void sleepMs(uint32_t ms) override { sleeps.push_back(ms); }
    void onEvent(const FwDlEvent& e) override { events.push_back(e); }
    size_t count(FwDlEventKind k) const {
        size_t n = 0;
        for (const auto& e : events) n += e.kind == k;
        return n;
    }
};

std::string manifestJson(const Bytes& bin, const Bytes& assets, const char* kit = "full", int rev = kRev,
                         const char* channel = "stable") {
    return std::string("{\"name\":\"full-1.3.0\",\"kit\":\"") + kit + "\",\"version\":\"v1.3.0\",\"board_rev\":" +
           std::to_string(rev) + ",\"channel\":\"" + channel + "\",\"tier\":\"official\",\"tag\":\"v1.3.0\"," +
           "\"files\":{\"CrossPad.bin\":{\"size\":" + std::to_string(bin.size()) + ",\"sha256\":\"" + hashHex(bin) +
           "\"},\"assets.bin\":{\"size\":" + std::to_string(assets.size()) + ",\"sha256\":\"" + hashHex(assets) +
           "\"}}}";
}

void setName(char* out, size_t cap, const std::string& s) { std::snprintf(out, cap, "%s", s.c_str()); }

FwCatalogVersion catalogVersion(const Bytes& bin, const Bytes& assets) {
    FwCatalogVersion v{};
    setName(v.version, sizeof v.version, "1.3.0");
    setName(v.tag, sizeof v.tag, "v1.3.0");
    v.channel = FwChannel::Stable;
    v.tier = FwTier::Official;
    setName(v.bin.name, sizeof v.bin.name, "CrossPad-v1.3.0-v2-full.bin");
    v.bin.size = static_cast<uint32_t>(bin.size());
    setName(v.assets.name, sizeof v.assets.name, "CrossPad-v1.3.0-v2-full-assets.bin");
    v.assets.size = static_cast<uint32_t>(assets.size());
    setName(v.manifest, sizeof v.manifest, "CrossPad-v1.3.0-v2-full-manifest.json");
    setName(v.sig, sizeof v.sig, "CrossPad-v1.3.0-v2-full-manifest.sig");
    return v;
}

// One release on the server, one card, one downloader.
struct Bench {
    Bytes bin = pattern(3 * kFwDlChunk + 123, 1);
    Bytes assets = pattern(kFwDlChunk + 7, 2);
    std::string manifest = manifestJson(bin, assets);
    FwCatalogVersion v = catalogVersion(bin, assets);
    FakeFiles files;
    FakeHttp http;
    FakeSha sha;
    FakePlatform platform{files};
    FwDownloader dl{platform, files, http, sha, kLayout};
    FwDlProgress progress;

    Bench() {
        http.serve(v.manifest, manifest);
        http.serve(v.sig, kGoodSig);
        http.serve(v.bin.name, bin);
        http.serve(v.assets.name, assets);
    }
    FwDlResult run(const char* slotA = nullptr, const char* slotB = nullptr) {
        return dl.run(kBase, "full", v, progress, slotA, slotB);
    }
    // A package an earlier run left whole on the card.
    void putComplete() {
        files.put(std::string(kPkg) + "/manifest.json", manifest);
        files.put(std::string(kPkg) + "/manifest.sig", kGoodSig);
        files.put(std::string(kPkg) + "/CrossPad.bin", bin);
        files.put(std::string(kPkg) + "/assets.bin", assets);
        files.put(std::string(kPkg) + "/.complete", "1\n");
    }
    bool packageWhole() const {
        return files.has(std::string(kPkg) + "/.complete") && !files.has(std::string(kPkg) + "/.downloading") &&
               files.files.at(std::string(kPkg) + "/CrossPad.bin") == bin &&
               files.files.at(std::string(kPkg) + "/assets.bin") == assets &&
               !files.has(std::string(kPkg) + "/CrossPad.bin.part") &&
               !files.has(std::string(kPkg) + "/assets.bin.part");
    }
    size_t opIndex(const std::string& op) const {
        for (size_t i = 0; i < files.ops.size(); ++i)
            if (files.ops[i] == op) return i;
        return SIZE_MAX;
    }
};

const std::string kBinPath = std::string(kPkg) + "/CrossPad.bin";
const std::string kBinPart = kBinPath + ".part";

} // namespace

TEST_CASE("download: a fresh package lands whole, markers in order", "[fw][download]") {
    Bench b;
    REQUIRE(b.run() == FwDlResult::Ok);
    CHECK(b.packageWhole());
    CHECK(b.files.text(std::string(kPkg) + "/manifest.json") == b.manifest);
    CHECK(b.files.text(std::string(kPkg) + "/manifest.sig") == kGoodSig);
    CHECK(b.files.text(std::string(kPkg) + "/.complete") == "1\n");
    CHECK(b.progress.done.load() == b.bin.size() + b.assets.size());
    CHECK(b.progress.total.load() == b.bin.size() + b.assets.size());

    REQUIRE(b.http.requests.size() == 4);
    CHECK(b.http.requests[0].url == FakeHttp::url(b.v.manifest));
    CHECK(b.http.requests[1].url == FakeHttp::url(b.v.sig));
    CHECK(b.http.requests[2].url == "http://test:8765/api/device/file/v1.3.0/CrossPad-v1.3.0-v2-full.bin");
    CHECK(b.http.requests[2].offset == 0);
    CHECK(b.http.requests[3].url == FakeHttp::url(b.v.assets.name));

    // .downloading before the first file, each file renamed from .part, .complete last, then .downloading goes.
    const size_t marker = b.opIndex("write " + std::string(kPkg) + "/.downloading");
    const size_t binPart = b.opIndex("write " + kBinPart);
    const size_t binDone = b.opIndex("rename " + kBinPart + " " + kBinPath);
    const size_t assetsDone =
        b.opIndex("rename " + std::string(kPkg) + "/assets.bin.part " + std::string(kPkg) + "/assets.bin");
    const size_t complete = b.opIndex("write " + std::string(kPkg) + "/.complete");
    const size_t unmark = b.opIndex("remove " + std::string(kPkg) + "/.downloading");
    CHECK(marker < binPart);
    CHECK(binPart < binDone);
    CHECK(binDone < assetsDone);
    CHECK(assetsDone < complete);
    CHECK(complete < unmark);
    CHECK(unmark != SIZE_MAX);

    // Room for the files and both manifest files, then the cleanup after the download.
    REQUIRE(b.platform.cleanups.size() == 2);
    CHECK(b.platform.cleanups[0] == b.bin.size() + b.assets.size() + b.manifest.size() + kGoodSig.size());
    CHECK(b.platform.cleanups[1] == 0);
    CHECK(b.platform.sleeps.empty());
    CHECK(b.platform.count(FwDlEventKind::Complete) == 1);
    CHECK(b.platform.allocs == 0);
    CHECK(b.files.openCount == 0);
    CHECK(b.platform.yields > 0);
}

TEST_CASE("download: a partial .part resumes with Range and is hashed whole", "[fw][download]") {
    Bench b;
    const size_t have = kFwDlChunk + 100;
    b.files.put(kBinPart, Bytes(b.bin.begin(), b.bin.begin() + have));
    REQUIRE(b.run() == FwDlResult::Ok);
    CHECK(b.packageWhole());
    REQUIRE(b.http.streamed(b.v.bin.name) == 1);
    CHECK(b.http.requests[2].offset == have);
    CHECK(b.opIndex("append " + kBinPart) != SIZE_MAX);
    CHECK(b.opIndex("write " + kBinPart) == SIZE_MAX);

    SECTION("a .part with foreign bytes fails the hash of the whole file") {
        Bench c;
        Bytes wrong(c.bin.begin(), c.bin.begin() + have);
        wrong[5] ^= 0xff;
        c.files.put(kBinPart, wrong);
        CHECK(c.run() == FwDlResult::Ok);   // the retry starts the file over
        CHECK(c.packageWhole());
        CHECK(c.platform.count(FwDlEventKind::ShaMismatch) == 1);
        REQUIRE(c.http.streamed(c.v.bin.name) == 2);
        CHECK(c.http.requests[2].offset == have);
        CHECK(c.http.requests[3].offset == 0);
    }
}

TEST_CASE("download: a connection that drops is resumed twice, 2 s apart, then the .part is kept",
          "[fw][download]") {
    Bench b;
    b.http.dropAt = kFwDlChunk + 50;
    b.http.drops = 1;
    REQUIRE(b.run() == FwDlResult::Ok);
    CHECK(b.packageWhole());
    REQUIRE(b.http.streamed(b.v.bin.name) == 2);
    CHECK(b.http.requests[3].offset == kFwDlChunk + 50);
    CHECK(b.platform.sleeps == std::vector<uint32_t>{kFwDlNetRetryDelayMs});
    CHECK(b.platform.count(FwDlEventKind::Resuming) == 1);

    Bench t;
    t.http.dropAt = 1000;
    t.http.drops = 3;
    // Every attempt ends at byte 1000: the first gets 1000, the resumes get none.
    CHECK(t.run() == FwDlResult::Network);
    CHECK(t.http.streamed(t.v.bin.name) == static_cast<size_t>(kFwDlNetAttempts));
    CHECK(t.platform.sleeps.size() == static_cast<size_t>(kFwDlNetAttempts - 1));
    CHECK(t.files.size(kBinPart.c_str()) == 1000);   // kept: the next run resumes
    CHECK_FALSE(t.files.has(std::string(kPkg) + "/.complete"));
    CHECK(t.files.has(std::string(kPkg) + "/.downloading"));
    CHECK_FALSE(t.platform.isBlocked("full", "1.3.0"));

    // The next run resumes the .part it left.
    t.http.drops = 0;
    t.http.requests.clear();
    CHECK(t.run() == FwDlResult::Ok);
    CHECK(t.packageWhole());
    REQUIRE(t.http.streamed(t.v.bin.name) == 1);
    CHECK(t.http.requests[2].offset == 1000);
}

TEST_CASE("download: a server that ignores Range starts the file over, never appends", "[fw][download]") {
    Bench b;
    b.files.put(kBinPart, Bytes(b.bin.begin(), b.bin.begin() + 500));
    b.http.ignoreRange = true;
    REQUIRE(b.run() == FwDlResult::Ok);
    CHECK(b.packageWhole());
    REQUIRE(b.http.streamed(b.v.bin.name) == 2);
    CHECK(b.http.requests[2].offset == 500);
    CHECK(b.http.requests[3].offset == 0);
    REQUIRE(b.platform.count(FwDlEventKind::ResumeRefused) == 1);
    for (const auto& e : b.platform.events)
        if (e.kind == FwDlEventKind::ResumeRefused) {
            CHECK(e.status == 200);
            CHECK(e.at == 500);
            CHECK_FALSE(e.otherRange);
        }
    CHECK(b.opIndex("remove " + kBinPart) != SIZE_MAX);

    SECTION("a 206 for another range") {
        Bench c;
        c.files.put(kBinPart, Bytes(c.bin.begin(), c.bin.begin() + 500));
        c.http.otherRange = true;
        REQUIRE(c.run() == FwDlResult::Ok);
        CHECK(c.packageWhole());
        CHECK(c.http.requests[3].offset == 0);
        for (const auto& e : c.platform.events)
            if (e.kind == FwDlEventKind::ResumeRefused) CHECK(e.otherRange);
    }
    SECTION("416") {
        Bench c;
        c.files.put(kBinPart, Bytes(c.bin.begin(), c.bin.begin() + 500));
        c.http.status416Once = 1;
        REQUIRE(c.run() == FwDlResult::Ok);
        CHECK(c.packageWhole());
        CHECK(c.platform.count(FwDlEventKind::RangeNotSatisfiable) == 1);
        CHECK(c.http.requests[3].offset == 0);
    }
    SECTION("a .part longer than the signed file is dropped before any request") {
        Bench c;
        c.files.put(kBinPart, pattern(c.bin.size() + 1, 9));
        REQUIRE(c.run() == FwDlResult::Ok);
        CHECK(c.packageWhole());
        REQUIRE(c.http.streamed(c.v.bin.name) == 1);
        CHECK(c.http.requests[2].offset == 0);
    }
    SECTION("an unreadable .part") {
        Bench c;
        c.files.put(kBinPart, Bytes(c.bin.begin(), c.bin.begin() + 500));
        c.files.unreadable.insert(kBinPart);
        REQUIRE(c.run() == FwDlResult::Ok);
        CHECK(c.packageWhole());
        CHECK(c.platform.count(FwDlEventKind::PartUnreadable) == 1);
        CHECK(c.http.requests[2].offset == 0);
    }
}

TEST_CASE("download: a SHA mismatch retries once fresh, then blocks the version and removes the package",
          "[fw][download]") {
    Bench b;
    Bytes bad = b.bin;
    bad[100] ^= 0x01;
    b.http.serve(b.v.bin.name, bad);
    CHECK(b.run() == FwDlResult::Sha);
    CHECK(b.http.streamed(b.v.bin.name) == static_cast<size_t>(kFwDlShaAttempts));
    CHECK(b.http.requests[2].offset == 0);
    CHECK(b.http.requests[3].offset == 0);
    CHECK(b.platform.count(FwDlEventKind::ShaMismatch) == static_cast<size_t>(kFwDlShaAttempts));
    CHECK(b.platform.isBlocked("full", "1.3.0"));
    CHECK(b.platform.removed == std::vector<std::string>{"full-1.3.0"});
    CHECK(b.files.under(kPkg).empty());

    // Blocked until reboot: the next run asks nobody anything.
    b.http.requests.clear();
    CHECK(b.run() == FwDlResult::Blocked);
    CHECK(b.http.requests.empty());

    SECTION("a package a slot holds stays on the card") {
        Bench c;
        c.http.serve(c.v.bin.name, bad);
        CHECK(c.run("FULL-1.3.0", nullptr) == FwDlResult::Sha);
        CHECK(c.platform.removed.empty());
        CHECK(c.files.has(std::string(kPkg) + "/manifest.json"));
        CHECK(c.platform.isBlocked("full", "1.3.0"));
    }
    SECTION("so does a pinned one") {
        Bench c;
        c.files.put(std::string(kPkg) + "/.pinned", "");
        c.http.serve(c.v.bin.name, bad);
        CHECK(c.run() == FwDlResult::Sha);
        CHECK(c.platform.removed.empty());
    }
    SECTION("more bytes than the manifest signed are a mismatch too") {
        Bench c;
        Bytes longer = c.bin;
        longer.push_back(0);
        c.http.serve(c.v.bin.name, longer);
        CHECK(c.run() == FwDlResult::Sha);
        CHECK(c.platform.isBlocked("full", "1.3.0"));
    }
}

TEST_CASE("download: a whole package is not fetched again", "[fw][download]") {
    Bench b;
    b.putComplete();
    REQUIRE(b.run() == FwDlResult::Ok);
    CHECK(b.http.requests.empty());
    CHECK(b.files.ops.empty());
    CHECK(b.progress.done.load() == b.progress.total.load());
    CHECK(b.dl.verifyOnCard("full", "1.3.0"));
    CHECK(b.dl.verifyOnCard("full", "v1.3.0") == false);   // another directory name

    SECTION("one that no longer verifies is downloaded again, its whole files kept") {
        Bench c;
        c.putComplete();
        c.files.put(std::string(kPkg) + "/manifest.sig", "tampered");
        CHECK_FALSE(c.dl.verifyOnCard("full", "1.3.0"));
        REQUIRE(c.run() == FwDlResult::Ok);
        CHECK(c.packageWhole());
        CHECK(c.platform.count(FwDlEventKind::NoLongerVerifies) == 1);
        CHECK(c.http.streamed(c.v.bin.name) == 0);   // already in place
        CHECK(c.http.streamed(c.v.assets.name) == 0);
        // .downloading goes in before .complete comes out.
        CHECK(c.opIndex("write " + std::string(kPkg) + "/.downloading") <
              c.opIndex("remove " + std::string(kPkg) + "/.complete"));
    }
    SECTION("a card that is full anyway leaves it complete") {
        Bench c;
        c.putComplete();
        c.files.put(std::string(kPkg) + "/manifest.sig", "tampered");
        c.platform.room = FwDlResult::CardFull;
        CHECK(c.run() == FwDlResult::CardFull);
        CHECK(c.files.has(std::string(kPkg) + "/.complete"));
        CHECK(c.files.ops.empty());
    }
}

TEST_CASE("download: a bad signature or a foreign manifest leaves the card alone", "[fw][download]") {
    Bench b;
    b.http.serve(b.v.sig, std::string("forged"));
    CHECK(b.run() == FwDlResult::Signature);
    CHECK(b.platform.isBlocked("full", "1.3.0"));
    CHECK(b.platform.count(FwDlEventKind::BadSignature) == 1);
    CHECK(b.files.under(kPkg).empty());
    CHECK(b.platform.cleanups.empty());

    Bench m;
    m.http.serve(m.v.manifest, manifestJson(m.bin, m.assets, "full", 1));
    CHECK(m.run() == FwDlResult::Mismatch);
    CHECK_FALSE(m.platform.isBlocked("full", "1.3.0"));
    CHECK(m.platform.count(FwDlEventKind::NotThisBoard) == 1);
    CHECK(m.files.under(kPkg).empty());

    Bench beta;
    beta.http.serve(beta.v.manifest, manifestJson(beta.bin, beta.assets, "full", kRev, "beta"));
    CHECK(beta.run() == FwDlResult::Mismatch);

    Bench sized;
    sized.v.bin.size += 1;
    CHECK(sized.run() == FwDlResult::Mismatch);

    Bench missing;
    missing.http.forcedStatus[FakeHttp::url(missing.v.sig)] = 404;
    CHECK(missing.run() == FwDlResult::Http);
    missing.http.forcedStatus.clear();
    missing.http.bodies.erase(FakeHttp::url(missing.v.manifest));
    CHECK(missing.run() == FwDlResult::Network);
}

TEST_CASE("download: catalog names, memory, card and HTTP refusals", "[fw][download]") {
    Bench b;
    setName(b.v.bin.name, sizeof b.v.bin.name, "../CrossPad.bin");
    CHECK(b.run() == FwDlResult::Mismatch);
    CHECK(b.platform.count(FwDlEventKind::UnsafeName) == 1);
    CHECK(b.http.requests.empty());
    CHECK(b.dl.run(kBase, nullptr, b.v, b.progress, nullptr, nullptr) == FwDlResult::Mismatch);
    CHECK(std::strcmp(b.platform.events.back().subject, "?") == 0);

    Bench mem;
    mem.platform.outOfMemory = true;
    CHECK(mem.run() == FwDlResult::NoMem);
    CHECK_FALSE(mem.dl.verifyOnCard("full", "1.3.0"));

    Bench full;
    full.platform.room = FwDlResult::CardFull;
    CHECK(full.run() == FwDlResult::CardFull);
    CHECK(full.files.under(kPkg).empty());

    Bench io;
    io.files.failWritePath = kBinPart;
    CHECK(io.run() == FwDlResult::Io);
    CHECK_FALSE(io.platform.isBlocked("full", "1.3.0"));

    Bench gone;
    gone.http.forcedStatus[FakeHttp::url(gone.v.assets.name)] = 404;
    CHECK(gone.run() == FwDlResult::Http);
    CHECK(gone.files.files.at(kBinPath) == gone.bin);   // the finished file stays for the next run
    gone.http.forcedStatus.clear();
    gone.http.requests.clear();
    CHECK(gone.run() == FwDlResult::Ok);
    CHECK(gone.packageWhole());
    CHECK(gone.http.streamed(gone.v.bin.name) == 0);
}

TEST_CASE("download: names, URLs and the blocklist", "[fw][download]") {
    char out[kFwFileUrlMax];
    REQUIRE(fwCatalogFileUrl("https://crosspad.app", "v1.3.0", "a.bin", out, sizeof out));
    CHECK(std::string(out) == "https://crosspad.app/api/device/file/v1.3.0/a.bin");
    CHECK_FALSE(fwCatalogFileUrl("https://crosspad.app", "v1.3.0", "a.bin", out, 20));
    char name[kFwPackageNameMax];
    fwPackageName("full", "1.3.0", name, sizeof name);
    CHECK(std::string(name) == "full-1.3.0");
    CHECK(std::string(fwDlResultName(FwDlResult::CardFull)) == "card_full");
    CHECK(std::string(fwDlResultName(FwDlResult::NoMem)) == "no_mem");

    FwBlocklist list;
    FwBlockedVersion forgotten{};
    bool dropped = true;
    CHECK(list.add("full", "1.0.0", &forgotten, &dropped));
    CHECK_FALSE(dropped);
    CHECK_FALSE(list.add("full", "1.0.0", &forgotten, &dropped));
    for (size_t i = 1; i < kFwBlocklistMax; ++i) {
        const std::string version = "1." + std::to_string(i) + ".0";
        CHECK(list.add("full", version.c_str(), &forgotten, &dropped));
        CHECK_FALSE(dropped);
    }
    CHECK(list.add("lite", "2.0.0", &forgotten, &dropped));
    CHECK(dropped);
    CHECK(std::string(forgotten.version) == "1.0.0");
    CHECK_FALSE(list.has("full", "1.0.0"));
    CHECK(list.has("full", "1.1.0"));
    CHECK(list.has("lite", "2.0.0"));
}

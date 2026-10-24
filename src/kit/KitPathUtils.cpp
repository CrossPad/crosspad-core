// SPDX-License-Identifier: GPL-3.0-or-later
#include <crosspad/kit/KitPathUtils.hpp>
#include <crosspad/platform/IFileSystem.hpp>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>

namespace crosspad {

namespace {

/* Callers pass logical paths, but choosing between the two layouts means
 * asking the filesystem, which needs the mount prefix. */
std::string g_fsPrefix;

bool pathExists(const std::string& path) {
    return getFileSystem().exists(path.c_str());
}

std::string parentDir(const std::string& path) {
    auto pos = path.rfind('/');
    return (pos != std::string::npos) ? path.substr(0, pos) : "";
}

/* What a kit's folder and its SAMPLES/ hold, read once.
 *
 * Every sample of a kit is resolved three or four times while it loads (the
 * take, the rate converter, the waveform loader, the selector's preview), and
 * each resolution of a SAMPLES/ kit was two stats -- the flat path first.
 * On a FAT card a stat walks the whole path from the root, the kits folder's
 * long names a sector at a time: one 16-sample kit load was 466 single-sector
 * directory reads of the card, 0.4-1.1 s of it, while voices were streaming
 * (SD_TRACE, 23.10.2026). A listing reads each folder once.
 *
 * Only a hit is trusted: a name not in the listing -- a file written since,
 * or one whose case differs from the kit's JSON, which FAT does not mind --
 * goes the old way, by stat. Two kits (the outgoing and the incoming one
 * during a swap), a few seconds each. */
struct KitListing {
    std::string dir;
    std::vector<std::string> flat, nested;
    std::chrono::steady_clock::time_point at;
};
constexpr int kListings = 2;
constexpr auto kListingTtl = std::chrono::seconds(3);
constexpr size_t kListingMaxNames = 512;
std::mutex g_listMutex;
KitListing g_listings[kListings];
int g_listNext = 0;

bool collectFile(void* ctx, const char* name, bool isDir) {
    auto* names = static_cast<std::vector<std::string>*>(ctx);
    if (!isDir) names->emplace_back(name);
    return names->size() < kListingMaxNames;
}

bool collectAll(void* ctx, const char* name, bool isDir) {
    auto* names = static_cast<std::vector<std::string>*>(ctx);
    names->emplace_back(isDir ? std::string(name) + "/" : std::string(name));
    return names->size() < kListingMaxNames;
}

/// 1 flat, 2 nested, 0 not in the listing. Under g_listMutex.
int listingLookup(const std::string& dir, const std::string& file) {
    const auto now = std::chrono::steady_clock::now();
    KitListing* hit = nullptr;
    for (auto& l : g_listings) {
        if (l.dir == dir && now - l.at < kListingTtl) { hit = &l; break; }
    }
    if (!hit) {
        hit = &g_listings[g_listNext];
        g_listNext = (g_listNext + 1) % kListings;
        hit->dir = dir;
        hit->at = now;
        hit->flat.clear();
        hit->nested.clear();
        std::vector<std::string> top;
        getFileSystem().list((g_fsPrefix + dir).c_str(), &collectAll, &top);
        bool samples = false;
        for (auto& n : top) {
            if (n == "SAMPLES/") samples = true;
            else if (n.empty() || n.back() != '/') hit->flat.push_back(std::move(n));
        }
        if (samples) getFileSystem().list((g_fsPrefix + dir + "/SAMPLES").c_str(), &collectFile, &hit->nested);
        std::sort(hit->flat.begin(), hit->flat.end());
        std::sort(hit->nested.begin(), hit->nested.end());
    }
    if (std::binary_search(hit->flat.begin(), hit->flat.end(), file)) return 1;
    if (std::binary_search(hit->nested.begin(), hit->nested.end(), file)) return 2;
    return 0;
}

} // namespace

void setKitPathFsPrefix(const std::string& prefix) {
    g_fsPrefix = prefix;
    forgetKitListings();
}

void forgetKitListings() {
    std::lock_guard<std::mutex> l(g_listMutex);
    for (auto& k : g_listings) k.dir.clear();
}

std::string resolveKitSamplePath(const std::string& kitJsonPath,
                                 const std::string& sampleFile) {
    if (sampleFile.empty()) return "";
    if (sampleFile[0] == '/') return sampleFile;

    const std::string dir  = parentDir(kitJsonPath);
    const std::string flat = dir + "/" + sampleFile;
    if (sampleFile.find('/') == std::string::npos) {
        std::lock_guard<std::mutex> l(g_listMutex);
        const int where = listingLookup(dir, sampleFile);
        if (where == 1) return flat;
        if (where == 2) return dir + "/SAMPLES/" + sampleFile;
    }
    if (pathExists(g_fsPrefix + flat)) return flat;

    const std::string nested = dir + "/SAMPLES/" + sampleFile;
    if (pathExists(g_fsPrefix + nested)) return nested;

    /* Neither exists: return the flat form, so the caller's "file not found"
     * still names where a flat kit would have kept it. */
    return flat;
}

std::string kitFsPath(const std::string& logicalPath) {
    return logicalPath.empty() ? std::string() : g_fsPrefix + logicalPath;
}

std::string renderedSamplePath(const std::string& logicalPath, const SampleFx& fx) {
    if (!fx.any() || logicalPath.empty()) return logicalPath;
    const auto slash = logicalPath.rfind('/');
    const std::string dir  = slash != std::string::npos ? logicalPath.substr(0, slash) : "";
    std::string name = slash != std::string::npos ? logicalPath.substr(slash + 1) : logicalPath;
    const auto dot = name.rfind('.');
    if (dot != std::string::npos) name.resize(dot);
    if (fx.semitones) name += (fx.semitones > 0 ? "_p+" : "_p") + std::to_string(fx.semitones);
    if (fx.hpHz) name += "_h" + std::to_string(fx.hpHz);
    if (fx.lpHz) name += "_l" + std::to_string(fx.lpHz);
    if (fx.reverse) name += "_r";
    return dir + "/.fx/" + name + ".wav";
}

std::string kitAssetPath(const std::string& kitJsonPath,
                         const std::string& fileName) {
    return g_fsPrefix + parentDir(kitJsonPath) + "/" + fileName;
}

} // namespace crosspad

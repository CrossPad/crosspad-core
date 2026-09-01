// SPDX-License-Identifier: MIT
#include <crosspad/kit/PortableKitLoader.hpp>
#include <crosspad/kit/KitPathUtils.hpp>
#include "KitJsonParser.hpp"

#include <ArduinoJson.h>

#include <sys/stat.h>
#include <dirent.h>
#include <fstream>
#include <algorithm>

namespace crosspad {

// ── Helpers ─────────────────────────────────────────────────────────────────

static bool pathExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static bool isDir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static std::string parentDir(const std::string& path) {
    auto pos = path.rfind('/');
    return (pos != std::string::npos) ? path.substr(0, pos) : "";
}

/// Create directory and parents (like mkdir -p).
static bool mkdirRecursive(const std::string& path) {
    if (path.empty() || isDir(path)) return true;
    mkdirRecursive(parentDir(path));
    return mkdir(path.c_str(), 0755) == 0 || isDir(path);
}

// ── IKitManager (persistence) ───────────────────────────────────────────────

std::string PortableKitLoader::toFsPath(const std::string& logicalPath) const {
    if (rootPrefix_.empty()) return logicalPath;
    return rootPrefix_ + logicalPath;
}

std::string PortableKitLoader::toLogicalPath(const std::string& fsPath) const {
    if (rootPrefix_.empty()) return fsPath;
    if (fsPath.compare(0, rootPrefix_.size(), rootPrefix_) == 0)
        return fsPath.substr(rootPrefix_.size());
    return fsPath;
}

std::vector<KitInfo> PortableKitLoader::listKits(const std::string& rootDir) {
    kits_.clear();
    currentKitId_ = -1;
    // rootDir is logical ("/crosspad/kits"); add the mount prefix for POSIX.
    discoverKitsRecursive(toFsPath(rootDir), kits_);
    return kits_;
}

KitLoadResult PortableKitLoader::loadKit(KitInfo& kit) {
    KitLoadResult result;

    if (kit.path.empty()) {
        result.error = "Kit path is empty";
        return result;
    }

    if (!kit.parsed) {
        if (!readKitJson(toFsPath(kit.path), kit, true)) {
            result.error = "Failed to read kit JSON";
            return result;
        }
    }

    int id = findKitIdByPath(kit.path);
    if (id >= 0) {
        kits_[id] = kit;
        // A kit that is no longer current drops its parsed pads. The payload
        // is hundreds of small std::strings, which on ESP32-S3 come out of
        // internal RAM under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL — ~1.4 kB per
        // distinct kit, permanently, against the ~19 kB free once BLE is up.
        // Re-parsing costs one JSON read on the load task.
        //
        // The free is deferred to releaseRetiredKit(): done here it runs on
        // the loader thread, and the GUI reads getCurrentKit()->pads across
        // calls that block on the SD card.
        if (currentKitId_ >= 0 && currentKitId_ != id) {
            retiredKitId_ = currentKitId_;
        }
        currentKitId_ = id;
    }

    result.success = true;
    result.padsLoaded = KIT_PADS;
    return result;
}

void PortableKitLoader::releaseRetiredKit() {
    if (retiredKitId_ < 0) return;
    const int id = retiredKitId_;
    retiredKitId_ = -1;
    // A kit that became current again in the meantime is in use, not retired.
    if (id == currentKitId_) return;
    if (id < static_cast<int>(kits_.size())) releaseParsedPads(kits_[id]);
}

void PortableKitLoader::releaseParsedPads(KitInfo& kit) {
    if (!kit.parsed) return;

    // Unsaved edits live only here; a kit that was merely played does not.
    for (const auto& pad : kit.pads) {
        if (pad.dirty || pad.dirtyModified) return;
    }

    for (auto& pad : kit.pads) {
        pad.samples.clear();
        pad.samples.shrink_to_fit();   // clear() alone keeps the capacity
        pad.sampleCount = 0;
    }
    kit.total_samples = 0;
    // Metadata stays: the launcher lists it, and re-reading it per redraw
    // would put the SD card on the LVGL thread.
    kit.parsed = false;
}

bool PortableKitLoader::saveKit(const KitInfo& kit) {
    return writeKitJson(toFsPath(kit.path), kit);
}

// ── IKitManager ─────────────────────────────────────────────────────────────

int PortableKitLoader::getKitCount() const {
    return static_cast<int>(kits_.size());
}

KitInfo* PortableKitLoader::getKit(int id) {
    if (id >= 0 && id < static_cast<int>(kits_.size()))
        return &kits_[id];
    return nullptr;
}

KitInfo* PortableKitLoader::getCurrentKit() {
    return getKit(currentKitId_);
}

int PortableKitLoader::getCurrentKitId() const {
    return currentKitId_;
}

bool PortableKitLoader::isLoading() const {
    return false;
}

void PortableKitLoader::setCurrentKitId(int id) {
    currentKitId_ = id;
}

// ── Non-virtual helpers ─────────────────────────────────────────────────────

int PortableKitLoader::findKitIdByPath(const std::string& path) const {
    for (int i = 0; i < static_cast<int>(kits_.size()); i++) {
        if (kits_[i].path == path) return i;
    }
    return -1;
}

bool PortableKitLoader::isKitKnown(const std::string& path) const {
    return findKitIdByPath(path) >= 0;
}

std::vector<KitInfo>& PortableKitLoader::kits() {
    return kits_;
}

const std::vector<KitInfo>& PortableKitLoader::kits() const {
    return kits_;
}

// ── Free function (KitPathUtils) ────────────────────────────────────────────

/* Callers pass logical paths, but choosing between the two layouts means
 * asking the filesystem, which needs the mount prefix. */
static std::string g_fsPrefix;

void setKitPathFsPrefix(const std::string& prefix) { g_fsPrefix = prefix; }

std::string resolveKitSamplePath(const std::string& kitJsonPath,
                                  const std::string& sampleFile) {
    if (sampleFile.empty()) return "";
    if (sampleFile[0] == '/') return sampleFile;

    const std::string dir  = parentDir(kitJsonPath);
    const std::string flat = dir + "/" + sampleFile;
    if (pathExists(g_fsPrefix + flat)) return flat;

    const std::string nested = dir + "/SAMPLES/" + sampleFile;
    if (pathExists(g_fsPrefix + nested)) return nested;

    /* Neither exists: return the flat form, so the caller's "file not found"
     * still names where a flat kit would have kept it. */
    return flat;
}

std::string kitAssetPath(const std::string& kitJsonPath,
                         const std::string& fileName) {
    return g_fsPrefix + parentDir(kitJsonPath) + "/" + fileName;
}

// ── Kit discovery ───────────────────────────────────────────────────────────

bool PortableKitLoader::discoverKitsRecursive(const std::string& dirPath,
                                               std::vector<KitInfo>& out) {
    DIR* dir = opendir(dirPath.c_str());
    if (!dir) return false;

    bool found = false;
    struct dirent* ent;

    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;

        std::string entryPath = dirPath + "/" + ent->d_name;

        if (!isDir(entryPath)) continue;

        std::string kitJsonPath = entryPath + "/kit.json";
        std::string altJsonPath = entryPath + "/" + ent->d_name + ".json";

        std::string jsonPath;
        if (pathExists(kitJsonPath)) {
            jsonPath = kitJsonPath;
        } else if (pathExists(altJsonPath)) {
            jsonPath = altJsonPath;
        }

        if (!jsonPath.empty()) {
            std::string logicalPath = toLogicalPath(jsonPath);
            if (!isKitKnown(logicalPath)) {
                KitInfo kit;
                kit.path = logicalPath;
                if (readKitJson(jsonPath, kit, false)) {
                    out.push_back(std::move(kit));
                    found = true;
                }
            }
        } else {
            // No kit.json here; recurse.
            found |= discoverKitsRecursive(entryPath, out);
        }
    }

    closedir(dir);
    return found;
}

// ── JSON I/O ────────────────────────────────────────────────────────────────

bool PortableKitLoader::readKitJson(const std::string& path, KitInfo& kit,
                                     bool parsePads) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    JsonDocument doc(jsonAllocator_
                         ? jsonAllocator_
                         : ArduinoJson::detail::DefaultAllocator::instance());
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) {
        printf("KitLoader: %s: %s\n", path.c_str(), err.c_str());
        return false;
    }

    // Not kit.path: the caller sets that to the logical path.
    kit_parse_metadata(kit, doc);

    if (parsePads) {
        kit_parse_pads(kit, doc);
    }

    return true;
}

bool PortableKitLoader::writeKitJson(const std::string& path,
                                      const KitInfo& kit) {
    mkdirRecursive(parentDir(path));

    std::ofstream file(path);
    if (!file.is_open()) return false;

    JsonDocument doc(jsonAllocator_
                         ? jsonAllocator_
                         : ArduinoJson::detail::DefaultAllocator::instance());
    kit_serialize(kit, doc);
    serializeJson(doc, file);
    file.close();

    return true;
}

} // namespace crosspad

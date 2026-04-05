// SPDX-License-Identifier: MIT
#include <crosspad/kit/PortableKitLoader.hpp>
#include <crosspad/kit/KitPathUtils.hpp>
#include <crosspad/kit/KitJsonParser.hpp>

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

// ── IKitLoader ──────────────────────────────────────────────────────────────

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
    // rootDir is a logical path (e.g. "/crosspad/kits"), add prefix for POSIX
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

    // Update the kit in our internal list if it exists there
    int id = findKitIdByPath(kit.path);
    if (id >= 0) {
        kits_[id] = kit;
        currentKitId_ = id;
    }

    result.success = true;
    result.padsLoaded = KIT_PADS;
    return result;
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

// ── Non-virtual helpers ─────────────────────────────────────────────────────

void PortableKitLoader::setCurrentKitId(int id) {
    currentKitId_ = id;
}

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

std::string PortableKitLoader::resolveRelativePath(const std::string& kitPath,
                                                    const std::string& sampleFile) {
    return resolveKitSamplePath(kitPath, sampleFile);
}

// ── Free function (KitPathUtils) ────────────────────────────────────────────

std::string resolveKitSamplePath(const std::string& kitJsonPath,
                                  const std::string& sampleFile) {
    if (sampleFile.empty()) return "";
    if (sampleFile[0] == '/') return sampleFile;
    return parentDir(kitJsonPath) + "/" + sampleFile;
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

        // Skip if already known
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
            // No kit.json here — recurse into subdirectory
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

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) return false;

    // Don't overwrite kit.path — caller sets it to the logical path
    kit_parse_metadata(kit, doc);

    if (parsePads) {
        kit_parse_pads(kit, doc);
    }

    return true;
}

bool PortableKitLoader::writeKitJson(const std::string& path,
                                      const KitInfo& kit) {
    // Ensure parent directory exists
    mkdirRecursive(parentDir(path));

    std::ofstream file(path);
    if (!file.is_open()) return false;

    JsonDocument doc;
    kit_serialize(kit, doc);
    serializeJson(doc, file);
    file.close();

    return true;
}

} // namespace crosspad

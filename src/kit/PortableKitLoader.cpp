// SPDX-License-Identifier: MIT
#include <crosspad/kit/PortableKitLoader.hpp>
#include <new>
#include <crosspad/kit/KitPathUtils.hpp>
#include "KitJsonParser.hpp"

#include <ArduinoJson.h>

#ifdef _WIN32
#include <filesystem>
#else
#include <sys/stat.h>
#include <dirent.h>
#endif
#include <fstream>
#include <algorithm>

namespace crosspad {

// ── Helpers ─────────────────────────────────────────────────────────────────

#ifdef _WIN32
// Windows has no dirent.h (the PC simulator built with MSVC).
static bool pathExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::u8path(path), ec);
}

static bool isDir(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_directory(std::filesystem::u8path(path), ec);
}

static bool makeDir(const std::string& path) {
    std::error_code ec;
    return std::filesystem::create_directory(std::filesystem::u8path(path), ec);
}

/// Entry names in a directory, hidden ones ('.') left out.
static bool listNames(const std::string& path, std::vector<std::string>& out) {
    std::error_code ec;
    std::filesystem::directory_iterator it(std::filesystem::u8path(path), ec);
    if (ec) return false;
    for (const auto& ent : it) {
        std::string name = ent.path().filename().u8string();
        if (!name.empty() && name[0] != '.') out.push_back(std::move(name));
    }
    return true;
}
#else
static bool pathExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static bool isDir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool makeDir(const std::string& path) {
    return mkdir(path.c_str(), 0755) == 0;
}

/// Entry names in a directory, hidden ones ('.') left out. The directory is
/// closed before the caller recurses, so a deep tree holds one handle.
static bool listNames(const std::string& path, std::vector<std::string>& out) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return false;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] != '.') out.emplace_back(ent->d_name);
    }
    closedir(dir);
    return true;
}
#endif

static std::string parentDir(const std::string& path) {
    auto pos = path.rfind('/');
    return (pos != std::string::npos) ? path.substr(0, pos) : "";
}

/// Create directory and parents (like mkdir -p).
static bool mkdirRecursive(const std::string& path) {
    if (path.empty() || isDir(path)) return true;
    mkdirRecursive(parentDir(path));
    return makeDir(path) || isDir(path);
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

void PortableKitLoader::scanKits(const std::string& rootDir) {
    // Growing by doubling means copying everything found so far, and on a card
    // of 79 kits of ~1 kB that copy is one of the largest allocations of the
    // boot: it threw bad_alloc on one boot in four, nobody caught it, and the
    // board rebooted with "Discovered 0 kits". The card rarely changes between
    // scans, so the last count is the best guess there is, and anything the
    // memory still cannot take costs its own entry rather than the boot.
    const size_t previous = kits_.size();
    kits_.clear();
    currentKitId_ = -1;
    try {
        kits_.reserve(previous ? previous + 4 : 32);
        // rootDir is logical ("/crosspad/kits"); add the mount prefix for POSIX.
        discoverKitsRecursive(toFsPath(rootDir), kits_);
        // Trim the slack the guess left — PSRAM outside the engine arena is the
        // UI's whole budget.
        kits_.shrink_to_fit();
    } catch (const std::bad_alloc&) {
        // Keep what was found. A short kit list is a bad boot; no boot is worse.
    }
}

std::vector<KitInfo> PortableKitLoader::listKits(const std::string& rootDir) {
    scanKits(rootDir);
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

// ── Kit discovery ───────────────────────────────────────────────────────────

bool PortableKitLoader::discoverKitsRecursive(const std::string& dirPath,
                                               std::vector<KitInfo>& out) {
    std::vector<std::string> names;
    if (!listNames(dirPath, names)) return false;

    bool found = false;

    for (const std::string& name : names) {
        std::string entryPath = dirPath + "/" + name;

        if (!isDir(entryPath)) continue;

        std::string kitJsonPath = entryPath + "/kit.json";
        std::string altJsonPath = entryPath + "/" + name + ".json";

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

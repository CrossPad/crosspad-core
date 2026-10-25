// SPDX-License-Identifier: GPL-3.0-or-later
#include <crosspad/kit/PortableKitLoader.hpp>
#include <new>
#include <crosspad/kit/KitPathUtils.hpp>
#include <crosspad/kit/KitSaved.hpp>
#include <crosspad/platform/IFileSystem.hpp>
#include "KitJsonParser.hpp"
#include "../util/JsonStdio.hpp"

#include <ArduinoJson.h>

#include <cerrno>
#include <cstdio>
#include <memory>
#include <algorithm>

namespace crosspad {

// ── Helpers ─────────────────────────────────────────────────────────────────

static bool pathExists(const std::string& path) {
    return getFileSystem().exists(path.c_str());
}

static bool isDir(const std::string& path) {
    return getFileSystem().isDir(path.c_str());
}

/// The folders in a directory, hidden ones ('.') left out. Read whole before
/// the caller recurses, so a deep tree holds one directory open.
static bool listSubdirs(const std::string& path, std::vector<std::string>& out) {
    return getFileSystem().forEach(path, [&](const char* name, bool dir) {
        if (dir && name[0] != '.') out.emplace_back(name);
    });
}

static std::string parentDir(const std::string& path) {
    auto pos = path.rfind('/');
    return (pos != std::string::npos) ? path.substr(0, pos) : "";
}

/// Create directory and parents (like mkdir -p).
static bool mkdirRecursive(const std::string& path) {
    if (path.empty() || isDir(path)) return true;
    mkdirRecursive(parentDir(path));
    return getFileSystem().makeDir(path.c_str());
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

// scanKits() exists beside listKits() because the vector listKits() returns
// is one contiguous block of kits x sizeof(KitInfo) plus a string copy per
// field: on the ESP32 that copy alone was a std::bad_alloc at boot (80 kits,
// 412 kB of PSRAM left beside the sample engine).
void PortableKitLoader::scanKits(const std::string& rootDir) {
    // One block of capacity * sizeof(KitInfo) (~1 kB a kit), reserved once at
    // the count plus kSpareKits -- the room addKit() fills without moving the
    // list. Grown by doubling, the block was copied into a bigger one with the
    // old one held: 88 kits wanted 64 and 128 kB at once, which under the
    // boot's update check threw and stopped the list at 64 kits. Short of one
    // block that size the spare goes first, then whole kits, and what is left
    // out is said.
    std::vector<KitInfo>().swap(kits_);   // the old block back before the new one is asked for
    currentKitId_ = -1;
    try {
        std::string found;   // every kit's JSON on the card, NUL-separated
        size_t count = 0;
        // rootDir is logical ("/crosspad/kits"); add the mount prefix for POSIX.
        findKitJsons(toFsPath(rootDir), found, count);
        size_t room = count + kSpareKits;
        for (;;) {
            try {
                kits_.reserve(room);
                break;
            } catch (const std::bad_alloc&) {
                room = room > count ? count : room / 2;
            }
        }
        for (size_t at = 0; at < found.size() && kits_.size() < kits_.capacity();) {
            const std::string jsonPath = found.c_str() + at;
            at += jsonPath.size() + 1;
            KitInfo kit;
            kit.path = toLogicalPath(jsonPath);
            if (!isKitKnown(kit.path) && readKitJson(jsonPath, kit, false)) kits_.push_back(std::move(kit));
        }
        if (kits_.capacity() < count) {
            std::printf("PortableKitLoader: no memory for the whole kit list: %u of %u kits listed\n",
                        static_cast<unsigned>(kits_.size()), static_cast<unsigned>(count));
        }
    } catch (const std::bad_alloc&) {
        // Keep what was found. A short kit list is a bad boot; no boot is worse.
        std::printf("PortableKitLoader: kit scan stopped by bad_alloc at %u kits\n",
                    static_cast<unsigned>(kits_.size()));
    }
}

int PortableKitLoader::addKit(const std::string& kitJsonPath) {
    const int known = findKitIdByPath(kitJsonPath);
    if (known >= 0) return known;
    KitInfo kit;
    kit.path = kitJsonPath;
    if (!readKitJson(toFsPath(kitJsonPath), kit, false)) return -1;
    if (kits_.size() < kits_.capacity()) {
        kits_.push_back(std::move(kit));
        return static_cast<int>(kits_.size()) - 1;
    }
    // No spare room left: the slot of a kit deleted since the scan. Nothing
    // holds that id any more -- removeKit() never blanks the current or the
    // retired kit.
    for (size_t i = 0; i < kits_.size(); ++i) {
        if (!kits_[i].path.empty()) continue;
        kits_[i] = std::move(kit);
        return static_cast<int>(i);
    }
    return -1;
}

// Not saveKit(): a kit this board has only ever listed carries sixteen empty
// pads, so serialising its KitInfo would take every sample out of the kit.
bool PortableKitLoader::saveKitInfo(int id, const KitInfo& meta) {
    if (id < 0 || id >= static_cast<int>(kits_.size())) return false;
    KitInfo& entry = kits_[id];
    if (entry.path.empty()) return false;

    /* The file as it stands, pads and all: four strings are being changed and
     * everything else has to go back exactly as it came out.
     *
     * On the heap, not on the stack: a KitInfo is the best part of a kilobyte
     * with its sixteen pads, and this runs inside a click callback on the UI
     * thread, whose stack is shared with everything LVGL does under it. */
    std::unique_ptr<KitInfo> onCard(new (std::nothrow) KitInfo());
    if (!onCard) return false;
    onCard->path = entry.path;
    if (!readKitJson(toFsPath(entry.path), *onCard, true)) return false;
    onCard->name        = meta.name;
    onCard->author      = meta.author;
    onCard->description = meta.description;
    onCard->bpm         = meta.bpm;
    if (!writeKitJson(toFsPath(entry.path), *onCard)) return false;

    /* The listed entry says the same as the file now -- without taking the
     * parsed pads of a kit that is loaded, which onCard holds its own copy of. */
    entry.name        = meta.name;
    entry.author      = meta.author;
    entry.description = meta.description;
    entry.bpm         = meta.bpm;
    return true;
}

bool PortableKitLoader::readKitCopy(const std::string& kitJsonPath, KitInfo& out) {
    out = KitInfo{};
    if (kitJsonPath.empty()) return false;
    out.path = kitJsonPath;
    return readKitJson(toFsPath(kitJsonPath), out, true);
}

bool PortableKitLoader::removeKit(int id) {
    forgetKitListings();
    if (id < 0 || id >= static_cast<int>(kits_.size())) return false;
    if (id == currentKitId_ || id == retiredKitId_) return false;
    const std::string path = kits_[id].path;
    if (path.empty()) return false;

    const std::string dir = parentDir(path);
    if (dir.empty() || dir == "/") return false;
    const std::string under = dir + "/";
    for (size_t i = 0; i < kits_.size(); ++i) {
        if (static_cast<int>(i) == id || kits_[i].path.empty()) continue;
        if (kits_[i].path.compare(0, under.size(), under) == 0) return false;
    }

    /* Everything under it, ".fx" -- the copies rendered for its pads -- too. */
    if (!getFileSystem().removeTree(toFsPath(dir))) return false;
    kits_[id] = KitInfo{};     // blanked, not erased: every id and pointer holds
    return true;
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

// Kept out of loadKit(): freeing from the loader thread pulled the samples
// vector out from under a GUI thread parked mid-callback, which surfaced as a
// null dereference at samples[i].end. The reader thread calls this instead.
void PortableKitLoader::releaseRetiredKit() {
    if (retiredKitId_ < 0) return;
    const int id = retiredKitId_;
    retiredKitId_ = -1;
    // A kit that became current again in the meantime is in use, not retired.
    if (id == currentKitId_) return;
    if (id < static_cast<int>(kits_.size())) releaseParsedPads(kits_[id]);
}

void PortableKitLoader::clearCurrentKit() {
    if (currentKitId_ < 0) return;
    retiredKitId_ = currentKitId_;
    currentKitId_ = -1;
    releaseRetiredKit();
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

void PortableKitLoader::findKitJsons(const std::string& dirPath, std::string& found,
                                     size_t& count) {
    std::vector<std::string> names;
    if (!listSubdirs(dirPath, names)) return;

    for (const std::string& name : names) {
        std::string entryPath = dirPath + "/" + name;

        std::string kitJsonPath = entryPath + "/kit.json";
        std::string altJsonPath = entryPath + "/" + name + ".json";

        /* A save cut between its two renames leaves only <file>.bak (see
         * writeKitJson); readKitJson() falls back to it. A lone .tmp may be
         * partial and is not a kit. */
        const auto present = [](const std::string& p) {
            return pathExists(p) || pathExists(p + ".bak");
        };
        std::string jsonPath;
        if (present(kitJsonPath)) {
            jsonPath = kitJsonPath;
        } else if (present(altJsonPath)) {
            jsonPath = altJsonPath;
        }

        if (!jsonPath.empty()) {
            found += jsonPath;
            found += '\0';
            ++count;
        } else {
            // No kit.json here; recurse.
            findKitJsons(entryPath, found, count);
        }
    }
}

// ── JSON I/O ────────────────────────────────────────────────────────────────

bool PortableKitLoader::readKitJson(const std::string& path, KitInfo& kit,
                                     bool parsePads) {
    FILE* file = std::fopen(path.c_str(), "r");
    // Only a missing file falls back: what writeKitJson leaves when cut short.
    if (!file && errno == ENOENT) file = std::fopen((path + ".bak").c_str(), "r");
    if (!file) return false;

    JsonDocument doc(jsonAllocator_
                         ? jsonAllocator_
                         : ArduinoJson::detail::DefaultAllocator::instance());
    JsonFileReader in(file);
    DeserializationError err = deserializeJson(doc, in);
    std::fclose(file);

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

    JsonDocument doc(jsonAllocator_
                         ? jsonAllocator_
                         : ArduinoJson::detail::DefaultAllocator::instance());
    kit_serialize(kit, doc);

    /* Written beside the real file and swapped in only once it is complete:
     * a full card or a pulled one costs this save, never the kit already on
     * the card. Opened only now, so nothing that can throw runs while the
     * handle is held. */
    const std::string tmp = path + ".tmp";
    FILE* file = std::fopen(tmp.c_str(), "w");
    if (!file) return false;
    JsonFileWriter out{file};
    serializeJson(doc, out);
    if (!closeWritten(file)) { std::remove(tmp.c_str()); return false; }

    /* rename() will not replace a file on FatFs, so the old one steps aside
     * first -- as .bak, not removed, so there is never a moment with neither.
     * With no kit.json (a save cut short before), the .bak is the kit: it
     * stays until the new file is in. */
    const std::string bak = path + ".bak";
    bool hadOld = false;
    if (pathExists(path)) {
        std::remove(bak.c_str());
        hadOld = std::rename(path.c_str(), bak.c_str()) == 0;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        if (hadOld) std::rename(bak.c_str(), path.c_str());
        std::remove(tmp.c_str());
        return false;
    }
    std::remove(bak.c_str());
    kit_saved_notify(toLogicalPath(path));
    return true;
}

} // namespace crosspad

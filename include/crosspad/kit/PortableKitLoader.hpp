// SPDX-License-Identifier: MIT
#pragma once

#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitPathUtils.hpp>
#include <ArduinoJson.h>
#include <string>
#include <vector>

namespace crosspad {

/// Platform-agnostic kit loader using POSIX + stdio.
/// Handles kit discovery, JSON I/O, and in-memory kit management.
/// Does NOT load audio samples — platform wrappers add that.
///
/// rootPrefix is a VFS mount-point prefix (e.g. "/sdcard" on ESP32, "" on PC).
/// It is added to paths for actual file I/O but NOT stored in KitInfo::path,
/// so kit paths remain portable across platforms.
class PortableKitLoader : public IKitManager {
public:
    explicit PortableKitLoader(const std::string& rootPrefix = "")
        : rootPrefix_(rootPrefix) { setKitPathFsPrefix(rootPrefix_); }

    // ── IKitManager (runtime state) ─────────────────────────────────────────
    int      getKitCount() const override;
    KitInfo* getKit(int id) override;
    KitInfo* getCurrentKit() override;
    int      getCurrentKitId() const override;
    void     setCurrentKitId(int id) override;
    bool     isLoading() const override;

    // ── IKitManager (persistence) ───────────────────────────────────────────
    std::vector<KitInfo> listKits(const std::string& rootDir) override;
    void                 scanKits(const std::string& rootDir) override;
    KitLoadResult loadKit(KitInfo& kit) override;
    bool saveKit(const KitInfo& kit) override;

    // ── IKitManager (memory) ───────────────────────────────────────
    void releaseRetiredKit() override;

    // ── Non-virtual helpers ─────────────────────────────────────────────────
    /// One kit written while the board runs (the Slicer's), onto the end of
    /// the list without a rescan. @p kitJsonPath is logical
    /// ("/crosspad/kits/<name>/kit.json"). Only into the spare room a scan
    /// leaves (kSpareKits): growing the vector would move every KitInfo, and
    /// the GUI holds getCurrentKit() across card reads. Returns the new id,
    /// or -1 when there is no room -- a rescan (reboot, card reinsert) finds it.
    int  addKit(const std::string& kitJsonPath) override;
    bool saveKitInfo(int id, const KitInfo& meta) override;
    bool removeKit(int id) override;
    static constexpr size_t kSpareKits = 8;
    int  findKitIdByPath(const std::string& path) const;
    bool isKitKnown(const std::string& path) const;
    std::vector<KitInfo>& kits();
    const std::vector<KitInfo>& kits() const;

    /// Set or change the root prefix (e.g. "/sdcard").
    void setRootPrefix(const std::string& prefix) {
        rootPrefix_ = prefix;
        setKitPathFsPrefix(rootPrefix_);
    }
    const std::string& rootPrefix() const { return rootPrefix_; }

    /// Optional allocator for the transient kit JSON documents. Embedded
    /// targets whose internal heap can't hold a full kit parse pass a
    /// PSRAM-backed allocator; nullptr (default) uses the ArduinoJson default.
    void setJsonAllocator(ArduinoJson::Allocator* alloc) { jsonAllocator_ = alloc; }

protected:
    ArduinoJson::Allocator* jsonAllocator_ = nullptr;
    std::string rootPrefix_;
    std::vector<KitInfo> kits_;
    int currentKitId_ = -1;

    /// Kit whose pads are still parsed but which is no longer current.
    /// Held until releaseRetiredKit() is called from the reader thread.
    int retiredKitId_ = -1;

    /// Prepend rootPrefix to a logical path for actual file I/O.
    std::string toFsPath(const std::string& logicalPath) const;

    /// Strip rootPrefix from a filesystem path to get the logical path.
    std::string toLogicalPath(const std::string& fsPath) const;

    /// Recursively scan dirPath for kit.json files, appending to out.
    bool discoverKitsRecursive(const std::string& dirPath,
                               std::vector<KitInfo>& out);

    /// Read and parse a kit JSON file.
    bool readKitJson(const std::string& path, KitInfo& kit, bool parsePads);

    /// Serialize and write a kit JSON file (creates parent dirs if needed).
    bool writeKitJson(const std::string& path, const KitInfo& kit);

    /// Free the sample payload of a kit that is no longer current, so it can
    /// be re-parsed on demand. Metadata survives; a kit holding unsaved pad
    /// edits is left alone. Called when the current kit changes.
    void releaseParsedPads(KitInfo& kit);
};

} // namespace crosspad

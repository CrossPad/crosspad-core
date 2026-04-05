// SPDX-License-Identifier: MIT
#pragma once

#include <crosspad/kit/IKitLoader.hpp>
#include <crosspad/kit/IKitManager.hpp>
#include <string>
#include <vector>

namespace crosspad {

/// Platform-agnostic kit loader using POSIX + std::fstream.
/// Handles kit discovery, JSON I/O, and in-memory kit management.
/// Does NOT load audio samples — platform wrappers add that.
///
/// rootPrefix is a VFS mount-point prefix (e.g. "/sdcard" on ESP32, "" on PC).
/// It is added to paths for actual file I/O but NOT stored in KitInfo::path,
/// so kit paths remain portable across platforms.
class PortableKitLoader : public IKitLoader, public IKitManager {
public:
    explicit PortableKitLoader(const std::string& rootPrefix = "")
        : rootPrefix_(rootPrefix) {}

    // ── IKitLoader ──────────────────────────────────────────────────────────
    std::vector<KitInfo> listKits(const std::string& rootDir) override;
    KitLoadResult loadKit(KitInfo& kit) override;
    bool saveKit(const KitInfo& kit) override;

    // ── IKitManager ─────────────────────────────────────────────────────────
    int      getKitCount() const override;
    KitInfo* getKit(int id) override;
    KitInfo* getCurrentKit() override;
    int      getCurrentKitId() const override;
    bool     isLoading() const override;

    // ── Non-virtual helpers ─────────────────────────────────────────────────
    void setCurrentKitId(int id);
    int  findKitIdByPath(const std::string& path) const;
    bool isKitKnown(const std::string& path) const;
    std::vector<KitInfo>& kits();
    const std::vector<KitInfo>& kits() const;

    /// Resolve a sample path relative to a kit.json location.
    /// If sampleFile starts with '/', returns it as-is.
    /// Otherwise prepends the kit's directory.
    static std::string resolveRelativePath(const std::string& kitPath,
                                           const std::string& sampleFile);

    /// Set or change the root prefix (e.g. "/sdcard").
    void setRootPrefix(const std::string& prefix) { rootPrefix_ = prefix; }
    const std::string& rootPrefix() const { return rootPrefix_; }

protected:
    std::string rootPrefix_;
    std::vector<KitInfo> kits_;
    int currentKitId_ = -1;

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
};

} // namespace crosspad

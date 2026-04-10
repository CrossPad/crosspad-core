#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <crosspad/kit/KitInfo.hpp>

namespace crosspad {

/// Result of a kit load operation.
struct KitLoadResult {
    bool success = false;
    std::string error;    ///< Empty on success, describes failure otherwise
    int padsLoaded = 0;   ///< Number of pads with valid sample references
};

/**
 * @brief Abstract interface for kit management and persistence.
 *
 * Provides platform-independent kit enumeration, selection, discovery,
 * loading, and saving. Platform implementations handle storage
 * (SD card, filesystem, etc.) and audio engine integration.
 */
class IKitManager {
public:
    virtual ~IKitManager() = default;

    // ── Runtime state ───────────────────────────────────────────────────────
    virtual int      getKitCount() const = 0;
    virtual KitInfo* getKit(int id) = 0;
    virtual KitInfo* getCurrentKit() = 0;
    virtual int      getCurrentKitId() const = 0;
    virtual void     setCurrentKitId(int id) = 0;
    virtual bool     isLoading() const = 0;

    // ── Persistence ─────────────────────────────────────────────────────────

    /// Scan rootDir for kit directories containing kit.json (or <dirname>.json).
    /// Returns metadata-only KitInfo entries (pads NOT parsed).
    virtual std::vector<KitInfo> listKits(const std::string& rootDir) = 0;

    /// Full-parse a kit: read JSON, parse metadata + pads array.
    /// kit.path must already be set before calling.
    virtual KitLoadResult loadKit(KitInfo& kit) = 0;

    /// Serialize kit metadata + pads back to JSON on disk.
    virtual bool saveKit(const KitInfo& kit) = 0;
};

} // namespace crosspad

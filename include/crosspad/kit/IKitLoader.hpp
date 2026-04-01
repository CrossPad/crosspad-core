// SPDX-License-Identifier: MIT
#pragma once

#include <crosspad/kit/KitInfo.hpp>
#include <string>
#include <vector>

namespace crosspad {

/// Result of a kit load operation.
struct KitLoadResult {
    bool success = false;
    std::string error;    ///< Empty on success, describes failure otherwise
    int padsLoaded = 0;   ///< Number of pads with valid sample references
};

/**
 * @brief Abstract interface for kit persistence operations.
 *
 * Handles discovery, loading, and saving of kit data on storage.
 * Separated from IKitManager (runtime state) for single responsibility.
 *
 * Paths are strings today; a future IKitRepository layer can route
 * them to filesystem, cloud, or other backends without changing this contract.
 */
class IKitLoader {
public:
    virtual ~IKitLoader() = default;

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

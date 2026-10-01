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

    /// Same scan, no copy: rebuilds the list read back through getKitCount()
    /// and getKit(). Use it when the caller only wants the card scanned — the
    /// vector listKits() returns is one contiguous block of
    /// kits × sizeof(KitInfo) plus a string copy per field, and on the ESP32
    /// that copy alone was a std::bad_alloc at boot (80 kits, 412 kB of PSRAM
    /// left beside the sample engine). Default keeps old implementations
    /// correct; they still pay for the copy.
    virtual void scanKits(const std::string& rootDir) { (void)listKits(rootDir); }

    /// Full-parse a kit: read JSON, parse metadata + pads array.
    /// kit.path must already be set before calling.
    virtual KitLoadResult loadKit(KitInfo& kit) = 0;

    /// Serialize kit metadata + pads back to JSON on disk.
    virtual bool saveKit(const KitInfo& kit) = 0;

    /// Take a kit.json written since the last scan into the list, so it is
    /// there without a restart; its id, or -1 when the list is full or the
    /// file will not parse. A path already in the list returns its id.
    /// Whoever wrote the file calls this -- the Slicer does, and so does the
    /// browser when it makes an empty kit.
    virtual int addKit(const std::string& kitJsonPath) { (void)kitJsonPath; return -1; }

    // ── Memory ────────────────────────────────────────────────────────

    /// Free the sample payload of the kit that stopped being current at
    /// the last loadKit(), if any.
    ///
    /// Deliberately not done inside loadKit(). A KitInfo* handed out by
    /// getCurrentKit() stays valid, but its pads[].samples vectors do not
    /// survive this call — and the GUI holds such a pointer across blocking
    /// reads, because drawing a waveform goes to the SD card. Freeing from
    /// the loader thread pulled the vector out from under a GUI thread
    /// parked mid-callback, which surfaces as a null dereference at
    /// samples[i].end. Call this from the same thread that reads kit pad
    /// data: then it can only run between callbacks, never inside one.
    ///
    /// Default is a no-op — an implementation that keeps every kit parsed
    /// has nothing to retire.
    virtual void releaseRetiredKit() {}
};

} // namespace crosspad

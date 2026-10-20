// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <crosspad/kit/KitInfo.hpp>

namespace crosspad {

/// Result of a kit load operation.
struct KitLoadResult {
    bool success = false; ///< true when the kit was read
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
    /// Number of entries in the kit list (removed kits included; see removeKit()).
    virtual int      getKitCount() const = 0;
    /// The kit at @p id in the list, or null when @p id is out of range.
    virtual KitInfo* getKit(int id) = 0;
    /// The kit that is playing now, or null when there is none.
    virtual KitInfo* getCurrentKit() = 0;
    /// The current kit's id, or -1 when there is none.
    virtual int      getCurrentKitId() const = 0;
    /// Make @p id the current kit's id.
    virtual void     setCurrentKitId(int id) = 0;
    /// True while a kit is being loaded.
    virtual bool     isLoading() const = 0;

    // ── Persistence ─────────────────────────────────────────────────────────

    /// Scan rootDir for kit directories containing kit.json (or <dirname>.json).
    /// Returns metadata-only KitInfo entries (pads NOT parsed).
    virtual std::vector<KitInfo> listKits(const std::string& rootDir) = 0;

    /// Same scan, no copy: rebuilds the list read back through getKitCount()
    /// and getKit(). Use it when the caller only wants the card scanned: the
    /// vector listKits() returns is one contiguous block of
    /// kits × sizeof(KitInfo) plus a string copy per field, which a card with
    /// many kits makes large. The default calls listKits().
    virtual void scanKits(const std::string& rootDir) { (void)listKits(rootDir); }

    /// Full-parse a kit: read JSON, parse metadata + pads array.
    /// kit.path must already be set before calling.
    virtual KitLoadResult loadKit(KitInfo& kit) = 0;

    /// Serialize kit metadata + pads back to JSON on disk.
    virtual bool saveKit(const KitInfo& kit) = 0;

    /// @p kitJsonPath's kit, pads and all, into @p out -- a copy of the
    /// caller's, the list and the current kit untouched: what a song's second
    /// kit is loaded from. Card I/O, so a worker's. False when it will not
    /// read, and where an implementation cannot do it.
    virtual bool readKitCopy(const std::string& kitJsonPath, KitInfo& out)
    {
        (void)kitJsonPath;
        (void)out;
        return false;
    }

    /// Take a kit.json written since the last scan into the list, so it is
    /// there without a restart; its id, or -1 when the list is full or the
    /// file will not parse. A path already in the list returns its id.
    /// Whoever wrote the file calls this -- the Slicer does, and so does the
    /// browser when it makes an empty kit.
    virtual int addKit(const std::string& kitJsonPath) { (void)kitJsonPath; return -1; }

    /// What a kit says about itself -- its name, who made it, what it is and
    /// the tempo it counts -- back into its own JSON. Only those four fields
    /// are taken from @p meta.
    ///
    /// Use this, not saveKit(), for a kit that has only been listed: saveKit()
    /// writes the KitInfo it is handed, pads and all, and a listed kit's pads
    /// are not parsed. Here the file is read first and its pads are written
    /// back exactly as they were.
    virtual bool saveKitInfo(int id, const KitInfo& meta) { (void)id; (void)meta; return false; }

    /// The kit at @p id gone: its JSON, the folder it had to itself with
    /// whatever it kept in there, and its place in the list. Refuses the kit
    /// that is current -- that one is playing -- and refuses a folder another
    /// listed kit lives under, which is the kits root or a pack and would take
    /// their kits with it.
    ///
    /// The entry is blanked rather than erased: a KitInfo* handed out by
    /// getKit() stays valid and no id shifts under code that cached one (see
    /// releaseRetiredKit() for what reads a kit across a card read). A blanked
    /// entry has an empty path, getKitCount() still counts it, and the next
    /// scan is simply without it -- so anything walking the list skips a kit
    /// whose path is empty.
    virtual bool removeKit(int id) { (void)id; return false; }

    // ── Memory ────────────────────────────────────────────────────────

    /// Free the sample payload of the kit that stopped being current at
    /// the last loadKit(), if any.
    ///
    /// Not done inside loadKit(). A KitInfo* handed out by getCurrentKit()
    /// stays valid, but its pads[].samples vectors do not survive this call,
    /// and the GUI holds such a pointer across blocking reads (drawing a
    /// waveform reads the SD card). Call this from the same thread that reads
    /// kit pad data, so it runs between callbacks, never inside one.
    ///
    /// Default is a no-op — an implementation that keeps every kit parsed
    /// has nothing to retire.
    virtual void releaseRetiredKit() {}

    /// No kit is current any more: what a song with no kit of its own opens
    /// to. The kit stays in the list, and its pads' parsed sample data is
    /// given back the way a kit that is no longer current gives it back
    /// (releaseRetiredKit(), so call it from the same thread). The engines
    /// holding its audio are the caller's to stop. Default: forget the id.
    virtual void clearCurrentKit() { setCurrentKitId(-1); }
};

} // namespace crosspad

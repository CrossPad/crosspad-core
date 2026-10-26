#pragma once

#include <string>

namespace crosspad {

/// Where kits live on the card, as logical paths. The kit list is scanned from
/// every one of them, and the folder a kit is in is the list of the kit browser
/// it is on: what the player sorted on the card is what the board shows.
struct KitFolders {
    static constexpr const char* kKits   = "/crosspad/kits";     ///< kits to play: PERF
    static constexpr const char* kDrums  = "/crosspad/drums";    ///< drum kits: DRUMS
    static constexpr const char* kSlicer = "/crosspad/slicer";   ///< what the Slicer made: SLICE
    static constexpr const char* kAll[] = {kKits, kDrums, kSlicer};

    /// @p kitPath (logical, ".../kit.json") is somewhere under @p folder.
    static bool in(const std::string& kitPath, const char* folder) {
        const std::string f = std::string(folder) + "/";
        return kitPath.compare(0, f.size(), f) == 0;
    }
};

} // namespace crosspad

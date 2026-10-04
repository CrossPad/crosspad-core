#pragma once
#include <cstdint>

namespace crosspad {

enum class FwSlotRole : uint8_t { None, Running, Other };

/// What a kit card knows about one version (platform fills it each refresh).
struct FwCardFacts {
    bool inCatalog;     ///< the catalog on the card lists this version for this board
    bool onCard;        ///< a package directory with this name exists
    bool complete;      ///< ... and is not a download in flight (.complete, or no .part)
    bool installable;   ///< the library verdict lets fw_install_start() take it
    FwSlotRole slot;    ///< where the image already is
    char slotLabel;     ///< 'A' / 'B' when slot != None
    bool pinned;
    bool busyThis;      ///< the updater is downloading/installing this version now
    bool readyThis;     ///< installed this boot, waiting for Run the new version
    bool updaterBusy;   ///< any updater job is running
};

enum FwCardAct : uint16_t {
    kFwActDownload = 1u << 0,
    kFwActResume = 1u << 1,
    kFwActInstallRun = 1u << 2,   ///< install the package on the card and restart into it (no network)
    kFwActInstall = 1u << 3,      ///< download, install into the other slot, then Run the new version
    kFwActRun = 1u << 4,          ///< the image is in the other slot: a plain switch
    kFwActRunNew = 1u << 5,
    kFwActPin = 1u << 6,
    kFwActUnpin = 1u << 7,
    kFwActDelete = 1u << 8,
};

/// Why an action is shown disabled (the card says it in words).
enum class FwCardBlock : uint8_t { None, InSlot, Busy, NotInstallable };

struct FwCardPlan {
    uint16_t shown;
    uint16_t enabled;
    FwCardBlock deleteBlock;
    FwCardBlock installBlock;
};

/// Which actions a kit card shows for a version, and which of them are enabled.
FwCardPlan fwCardPlan(const FwCardFacts& f);

} // namespace crosspad

#include "crosspad/fw/FwCardActions.hpp"

namespace crosspad {

FwCardPlan fwCardPlan(const FwCardFacts& f) {
    FwCardPlan p{};
    auto show = [&p](uint16_t act, bool enabled) {
        p.shown |= act;
        if (enabled) p.enabled |= act;
    };
    const bool idle = !f.updaterBusy;
    if (f.readyThis) show(kFwActRunNew, true);
    if (f.onCard) {
        show(f.pinned ? kFwActUnpin : kFwActPin, !f.busyThis);
        show(kFwActDelete, f.slot == FwSlotRole::None && !f.busyThis);
        if (f.slot != FwSlotRole::None) p.deleteBlock = FwCardBlock::InSlot;
        else if (f.busyThis) p.deleteBlock = FwCardBlock::Busy;
    }
    if (f.slot == FwSlotRole::Other && !f.readyThis) show(kFwActRun, idle);
    // An image already in a slot is run, never installed again.
    if (f.slot != FwSlotRole::None) return p;
    if (f.onCard && f.complete) {
        show(kFwActInstallRun, f.installable && idle);
        if (!f.installable) p.installBlock = FwCardBlock::NotInstallable;
    } else if (f.onCard) {
        if (f.inCatalog) show(kFwActResume, idle && !f.busyThis);
    } else if (f.inCatalog) {
        show(kFwActDownload, idle);
        show(kFwActInstall, idle);
    }
    return p;
}

} // namespace crosspad

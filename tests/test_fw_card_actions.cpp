#include <catch2/catch_test_macros.hpp>
#include "crosspad/fw/FwCardActions.hpp"

using namespace crosspad;

static FwCardFacts base() {
    FwCardFacts f{};
    f.slot = FwSlotRole::None;
    return f;
}

TEST_CASE("card: a catalog version not on the card offers Download and Install", "[fw]") {
    FwCardFacts f = base();
    f.inCatalog = true;
    FwCardPlan p = fwCardPlan(f);
    CHECK(p.shown == (kFwActDownload | kFwActInstall));
    CHECK(p.enabled == p.shown);
    f.updaterBusy = true;
    p = fwCardPlan(f);
    CHECK(p.shown == (kFwActDownload | kFwActInstall));
    CHECK(p.enabled == 0);                       // one job at a time
    f = base();                                  // a hand-made package the catalog never listed
    CHECK(fwCardPlan(f).shown == 0);
}

TEST_CASE("card: a complete package installs from the card, pins and deletes", "[fw]") {
    FwCardFacts f = base();
    f.onCard = f.complete = f.installable = true;
    f.inCatalog = true;                          // listed or not: the card is the source
    FwCardPlan p = fwCardPlan(f);
    CHECK(p.shown == (kFwActInstallRun | kFwActPin | kFwActDelete));
    CHECK(p.enabled == p.shown);
    f.updaterBusy = true;                        // fw_install_start() would refuse beside a job
    CHECK((fwCardPlan(f).enabled & kFwActInstallRun) == 0);
    f.updaterBusy = false;
    f.pinned = true;
    f.installable = false;
    p = fwCardPlan(f);
    CHECK((p.shown & kFwActUnpin) != 0);
    CHECK((p.enabled & kFwActInstallRun) == 0);
    CHECK(p.installBlock == FwCardBlock::NotInstallable);
}

TEST_CASE("card: an image in a slot is never deleted", "[fw]") {
    FwCardFacts f = base();
    f.onCard = f.complete = f.installable = true;
    f.slot = FwSlotRole::Running;
    f.slotLabel = 'A';
    FwCardPlan p = fwCardPlan(f);
    CHECK((p.shown & (kFwActInstallRun | kFwActRun)) == 0);
    CHECK((p.shown & kFwActDelete) != 0);
    CHECK((p.enabled & kFwActDelete) == 0);
    CHECK(p.deleteBlock == FwCardBlock::InSlot);
    f.slot = FwSlotRole::Other;
    p = fwCardPlan(f);
    CHECK((p.enabled & kFwActRun) != 0);
    CHECK(p.deleteBlock == FwCardBlock::InSlot);
}

TEST_CASE("card: an incomplete package resumes or is deleted", "[fw]") {
    FwCardFacts f = base();
    f.onCard = true;
    f.inCatalog = true;
    FwCardPlan p = fwCardPlan(f);
    CHECK(p.shown == (kFwActResume | kFwActPin | kFwActDelete));
    f.busyThis = f.updaterBusy = true;           // the updater is on it right now
    p = fwCardPlan(f);
    CHECK(p.enabled == 0);
    CHECK(p.deleteBlock == FwCardBlock::Busy);
}

TEST_CASE("card: installed this boot offers Run the new version only", "[fw]") {
    FwCardFacts f = base();
    f.onCard = f.complete = f.installable = f.inCatalog = true;
    f.slot = FwSlotRole::Other;
    f.readyThis = true;
    FwCardPlan p = fwCardPlan(f);
    CHECK((p.shown & kFwActRunNew) != 0);
    CHECK((p.shown & kFwActRun) == 0);
}

TEST_CASE("card: a pinned package is not offered Delete", "[fw]") {
    FwCardFacts f = base();
    f.onCard = f.complete = f.installable = true;
    f.pinned = true;
    FwCardPlan p = fwCardPlan(f);
    CHECK((p.shown & kFwActUnpin) != 0);
    CHECK((p.shown & kFwActDelete) == 0);
    f.slot = FwSlotRole::Running;                // pinned and running: still only Unpin
    p = fwCardPlan(f);
    CHECK((p.shown & kFwActUnpin) != 0);
    CHECK((p.shown & kFwActDelete) == 0);
}

TEST_CASE("card: the running image with no package on the card offers nothing", "[fw]") {
    FwCardFacts f = base();
    f.slot = FwSlotRole::Running;
    CHECK(fwCardPlan(f).shown == 0);
}

TEST_CASE("card: a download that is starting shows its actions disabled", "[fw]") {
    FwCardFacts f = base();
    f.inCatalog = f.busyThis = f.updaterBusy = true;   // the directory does not exist yet
    FwCardPlan p = fwCardPlan(f);
    CHECK(p.shown == (kFwActDownload | kFwActInstall));
    CHECK(p.enabled == 0);
}

TEST_CASE("card: Run the new version stays enabled beside a busy updater", "[fw]") {
    FwCardFacts f = base();
    f.onCard = f.complete = f.installable = f.inCatalog = true;
    f.slot = FwSlotRole::Other;
    f.readyThis = f.updaterBusy = true;
    FwCardPlan p = fwCardPlan(f);
    CHECK((p.enabled & kFwActRunNew) != 0);
    CHECK((p.shown & kFwActRun) == 0);
}

TEST_CASE("card: a package that cannot be installed says so even when busy", "[fw]") {
    FwCardFacts f = base();
    f.onCard = f.complete = true;
    f.updaterBusy = true;
    FwCardPlan p = fwCardPlan(f);
    CHECK((p.enabled & kFwActInstallRun) == 0);
    CHECK(p.installBlock == FwCardBlock::NotInstallable);
}

TEST_CASE("card: a rolled-back other slot is run again or reinstalled", "[fw]") {
    FwCardFacts f = base();
    f.onCard = f.complete = f.installable = f.inCatalog = true;
    f.slot = FwSlotRole::Other;
    f.slotLabel = 'B';
    f.slotRolledBack = true;
    FwCardPlan p = fwCardPlan(f);
    CHECK(p.shown == (kFwActRun | kFwActInstallRun | kFwActPin | kFwActDelete));
    CHECK(p.enabled == (kFwActRun | kFwActInstallRun | kFwActPin));
    CHECK(p.deleteBlock == FwCardBlock::InSlot);   // still in the slot: never deleted
    f.updaterBusy = true;
    CHECK((fwCardPlan(f).enabled & (kFwActRun | kFwActInstallRun)) == 0);
    f = base();                                    // only the slot holds it: fetched and written afresh
    f.inCatalog = true;
    f.slot = FwSlotRole::Other;
    f.slotLabel = 'B';
    f.slotRolledBack = true;
    CHECK(fwCardPlan(f).shown == (kFwActRun | kFwActDownload | kFwActInstall));
    f.slotRolledBack = false;                      // a slot that started is only run
    CHECK(fwCardPlan(f).shown == kFwActRun);
}

TEST_CASE("card: the running slot is never reinstalled", "[fw]") {
    FwCardFacts f = base();
    f.onCard = f.complete = f.installable = true;
    f.slot = FwSlotRole::Running;
    f.slotRolledBack = true;                       // not a state the platform reports; the rule holds anyway
    CHECK((fwCardPlan(f).shown & (kFwActInstallRun | kFwActRun)) == 0);
}

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

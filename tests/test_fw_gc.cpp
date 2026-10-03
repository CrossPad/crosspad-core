#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <vector>
#include "crosspad/fw/FwLibraryGc.hpp"

using namespace crosspad;

static constexpr uint64_t MB = 1024ull * 1024ull;

static FwGcPackage pkg(const char* name, const char* kit, const char* version, uint64_t mb, int64_t mtime,
                       bool prot = false) {
    FwGcPackage p{};
    std::strncpy(p.name, name, sizeof p.name - 1);
    std::strncpy(p.kit, kit, sizeof p.kit - 1);
    std::strncpy(p.version, version, sizeof p.version - 1);
    p.bytes = mb * MB;
    p.mtime = mtime;
    p.isProtected = prot;
    return p;
}

static const FwGcLimits kRoomy{512 * MB, 32000 * MB, 20000 * MB, 0};

TEST_CASE("cleanup does nothing within the limits", "[fw]") {
    std::vector<FwGcPackage> v{pkg("full-1.2.0", "full", "1.2.0", 4, 100)};
    FwGcPlan p = FwLibraryGc::select(v.data(), v.size(), {512 * MB, 32000 * MB, 20000 * MB, 4 * MB});
    CHECK(p.satisfiable);
    CHECK(p.count == 0);
}

TEST_CASE("cleanup takes the oldest version of a kit first, then the oldest directory", "[fw]") {
    std::vector<FwGcPackage> v{
        pkg("full-1.2.0", "full", "1.2.0", 200, 100),
        pkg("full-1.1.0", "full", "1.1.0", 200, 300),   // older version, newer directory
        pkg("lite-1.0.0", "lite", "1.0.0", 100, 200),
    };
    FwGcLimits lim = kRoomy;
    lim.incoming = 100 * MB;                            // 500 + 100 > 512: free at least 88 MB
    FwGcPlan p = FwLibraryGc::select(v.data(), v.size(), lim);
    REQUIRE(p.satisfiable);
    REQUIRE(p.count == 1);
    CHECK(p.evict[0] == 2);                             // kit heads: full-1.1.0 (mtime 300), lite-1.0.0 (200)
    lim.incoming = 250 * MB;                            // 750 > 512: free 238 MB
    p = FwLibraryGc::select(v.data(), v.size(), lim);
    REQUIRE(p.satisfiable);
    REQUIRE(p.count == 2);
    CHECK(p.evict[0] == 2);
    CHECK(p.evict[1] == 1);                             // then full's oldest version, not its oldest directory
}

TEST_CASE("cleanup never touches protected packages and refuses instead", "[fw]") {
    std::vector<FwGcPackage> v{
        pkg("slot-a", "full", "1.0.0", 300, 1, true),
        pkg("pinned", "full", "0.9.0", 150, 2, true),
        pkg("old", "full", "1.1.0", 50, 3),
    };
    FwGcLimits lim = kRoomy;
    lim.incoming = 100 * MB;                            // 600 > 512 even after "old" goes
    FwGcPlan p = FwLibraryGc::select(v.data(), v.size(), lim);
    CHECK_FALSE(p.satisfiable);
    CHECK(p.count == 0);                                // nothing deleted when it would not help
}

TEST_CASE("cleanup keeps 10 % of the card free", "[fw]") {
    std::vector<FwGcPackage> v{pkg("a", "full", "1.0.0", 40, 1), pkg("b", "full", "1.1.0", 40, 2)};
    // 1000 MB card, 120 MB free, 50 MB incoming: 70 MB left < 100 MB minimum -> free >= 30 MB.
    FwGcPlan p = FwLibraryGc::select(v.data(), v.size(), {512 * MB, 1000 * MB, 120 * MB, 50 * MB});
    REQUIRE(p.satisfiable);
    REQUIRE(p.count == 1);
    CHECK(p.evict[0] == 0);
}

TEST_CASE("loose images count as the oldest of their own group", "[fw]") {
    std::vector<FwGcPackage> v{pkg("full-1.0.0", "full", "1.0.0", 300, 10), pkg("stray.bin", "", "", 300, 50)};
    FwGcLimits lim = kRoomy;
    lim.incoming = 10 * MB;
    FwGcPlan p = FwLibraryGc::select(v.data(), v.size(), lim);
    REQUIRE(p.satisfiable);
    REQUIRE(p.count == 1);
    CHECK(p.evict[0] == 0);                             // both are heads of their group: older mtime goes first
}

TEST_CASE("an incoming package larger than the limit is refused", "[fw]") {
    FwGcLimits lim = kRoomy;
    lim.incoming = 600 * MB;
    FwGcPlan p = FwLibraryGc::select(nullptr, 0, lim);
    CHECK_FALSE(p.satisfiable);
}

TEST_CASE("cleanup skips a protected package even when it is the oldest", "[fw]") {
    std::vector<FwGcPackage> v{
        pkg("slot-a", "full", "1.0.0", 200, 1, true),
        pkg("mid", "full", "1.1.0", 200, 2),
        pkg("new", "full", "1.2.0", 100, 3),
    };
    FwGcLimits lim = kRoomy;
    lim.incoming = 100 * MB;                            // 500 + 100 > 512
    FwGcPlan p = FwLibraryGc::select(v.data(), v.size(), lim);
    REQUIRE(p.satisfiable);
    REQUIRE(p.count == 1);
    CHECK(p.evict[0] == 1);
}

TEST_CASE("cleanup arithmetic saturates instead of wrapping", "[fw]") {
    std::vector<FwGcPackage> v{pkg("a", "full", "1.0.0", 40, 1), pkg("b", "full", "1.1.0", 40, 2)};
    // Unlimited library, card pressure only: one eviction is enough.
    FwGcPlan p = FwLibraryGc::select(v.data(), v.size(), {UINT64_MAX, 1000 * MB, 120 * MB, 50 * MB});
    REQUIRE(p.satisfiable);
    REQUIRE(p.count == 1);
    CHECK(p.evict[0] == 0);
    // An incoming size that would wrap `incoming + minFree` is refused, not waved through.
    p = FwLibraryGc::select(v.data(), v.size(), {UINT64_MAX, 1000 * MB, 120 * MB, UINT64_MAX - 5});
    CHECK_FALSE(p.satisfiable);
    CHECK(p.count == 0);
}

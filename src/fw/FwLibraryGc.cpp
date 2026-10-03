#include "crosspad/fw/FwLibraryGc.hpp"
#include <cstdint>
#include <cstring>
#include "crosspad/fw/Semver.hpp"

namespace crosspad {
namespace {

constexpr uint64_t kPercent = 100;

uint64_t satAdd(uint64_t a, uint64_t b) { return a > UINT64_MAX - b ? UINT64_MAX : a + b; }

/// a older than b: version first (unparseable lowest), then directory time, then name.
bool olderThan(const FwGcPackage& a, const FwGcPackage& b) {
    const int c = compareVersionText(a.version, b.version);
    if (c != 0) return c < 0;
    if (a.mtime != b.mtime) return a.mtime < b.mtime;
    return std::strcmp(a.name, b.name) < 0;
}

bool candidate(const FwGcPackage* pkgs, const bool* taken, size_t i) {
    return !taken[i] && !pkgs[i].isProtected;
}

/// pkgs[i] is the oldest remaining candidate of its own kit.
bool kitHead(const FwGcPackage* pkgs, size_t n, const bool* taken, size_t i) {
    for (size_t j = 0; j < n; ++j) {
        if (j == i || !candidate(pkgs, taken, j)) continue;
        if (std::strcmp(pkgs[j].kit, pkgs[i].kit) == 0 && olderThan(pkgs[j], pkgs[i])) return false;
    }
    return true;
}

} // namespace

FwGcPlan FwLibraryGc::select(const FwGcPackage* pkgs, size_t n, const FwGcLimits& lim) {
    FwGcPlan plan{};
    if (n > kFwGcMaxPackages) n = kFwGcMaxPackages;
    uint64_t library = 0;
    for (size_t i = 0; i < n; ++i) library = satAdd(library, pkgs[i].bytes);
    const uint64_t minFree = lim.cardTotal / kPercent * kFwCardMinFreePct;
    uint64_t freed = 0;
    const auto fits = [&] {
        return satAdd(library, lim.incoming) <= satAdd(lim.libraryMax, freed) &&
               satAdd(lim.cardFree, freed) >= satAdd(lim.incoming, minFree);
    };
    bool taken[kFwGcMaxPackages] = {};
    while (!fits()) {
        int best = -1;
        for (size_t i = 0; i < n; ++i) {
            if (!candidate(pkgs, taken, i) || !kitHead(pkgs, n, taken, i)) continue;
            const bool earlier = best < 0 || pkgs[i].mtime < pkgs[best].mtime ||
                                 (pkgs[i].mtime == pkgs[best].mtime && std::strcmp(pkgs[i].name, pkgs[best].name) < 0);
            if (earlier) best = static_cast<int>(i);
        }
        if (best < 0) return FwGcPlan{};   // satisfiable=false, count=0
        taken[best] = true;
        freed = satAdd(freed, pkgs[best].bytes);
        plan.evict[plan.count++] = static_cast<uint16_t>(best);
    }
    plan.satisfiable = true;
    return plan;
}

} // namespace crosspad

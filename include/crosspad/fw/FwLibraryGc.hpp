#pragma once
#include <cstddef>
#include <cstdint>
#include "crosspad/fw/FwKit.hpp"

namespace crosspad {

constexpr uint64_t kFwLibraryMaxDefault = 512ull * 1024ull * 1024ull;
constexpr unsigned kFwCardMinFreePct = 10;
constexpr size_t kFwGcMaxPackages = 32;

/// One directory (or loose *.bin) in the firmware library.
struct FwGcPackage {
    char name[48];
    char kit[kFwKitMax];   ///< "" for a loose image or a directory without CPFI
    char version[32];      ///< "" sorts as the oldest
    uint64_t bytes;
    int64_t mtime;
    bool isProtected;      ///< in a slot, pinned, or being downloaded
};

struct FwGcLimits {
    uint64_t libraryMax;
    uint64_t cardTotal;
    uint64_t cardFree;
    uint64_t incoming;     ///< bytes about to be written; 0 after a download
};

struct FwGcPlan {
    bool satisfiable;
    size_t count;
    uint16_t evict[kFwGcMaxPackages];   ///< indices into the input, in deletion order
};

/// Space-only cleanup (spec §5): the smallest set of unprotected packages,
/// taken oldest version first within a kit and oldest directory across kits,
/// that brings the library under libraryMax and leaves kFwCardMinFreePct of
/// the card free with `incoming` written. Not satisfiable without touching a
/// protected package -> satisfiable=false and nothing to delete. Sums
/// saturate, so UINT64_MAX limits and huge `incoming` values do not wrap.
/// At most kFwGcMaxPackages packages are considered; the scanner must cap.
struct FwLibraryGc {
    static FwGcPlan select(const FwGcPackage* pkgs, size_t n, const FwGcLimits& lim);
};

} // namespace crosspad

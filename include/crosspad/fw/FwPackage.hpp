// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include "crosspad/fw/FwKit.hpp"

namespace crosspad {

/// "<kit>-<version>", bytes incl. NUL: the directory a catalog version downloads into.
constexpr size_t kFwPackageNameMax = 48;
void fwPackageName(const char* kit, const char* version, char* out, size_t n);
/// A version as an image or a package names it, incl. NUL.
constexpr size_t kFwPkgVersionMax = 32;

/// A package judged against the running image: only Ok installs, UnknownBuild
/// after a warning (an image built before CPFI); Downloading is a download in
/// flight (no .complete yet), never installable.
enum class FwPkgVerdict : uint8_t {
    Ok, NoImage, NotAnImage, WrongProject, WrongBoard, TooLarge, UnknownBuild, Downloading
};

/// The verdict in words for the user ("built for another board revision").
const char* fwPkgVerdictText(FwPkgVerdict v);

enum class FwSlotState : uint8_t { Empty, Installed, Trial, Confirmed, RolledBack };

/// What one OTA slot holds. The platform labels its two app partitions 'A' and
/// 'B', a fixed mapping independent of which one runs.
struct FwSlotInfo {
    char label;                     ///< 'A' | 'B'
    bool running;
    FwSlotState state;
    char package[kFwPackageNameMax];    ///< "" = installed by OTA, unnamed
    char version[kFwPkgVersionMax];
    char kit[kFwKitMax];
    bool hasDesc;                   ///< the image carries a CPFI block: it can confirm itself
    uint8_t boardRev;               ///< that block's board revision, 0 without one
};

} // namespace crosspad

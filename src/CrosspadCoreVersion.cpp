// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/CrosspadCoreVersion.hpp"

namespace crosspad {

LibVersion coreVersion()
{
    return {CROSSPAD_CORE_VERSION_MAJOR, CROSSPAD_CORE_VERSION_MINOR, CROSSPAD_CORE_VERSION_PATCH};
}

VersionMatch compareVersion(LibVersion have, uint8_t wantMajor, uint8_t wantMinor, uint8_t wantPatch)
{
    if (have.major != wantMajor) return VersionMatch::Incompatible;
    if (have.minor != wantMinor || have.patch != wantPatch) return VersionMatch::Drift;
    return VersionMatch::Exact;
}

} // namespace crosspad

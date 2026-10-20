// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/fw/FwPackage.hpp"
#include <cstdio>

namespace crosspad {

void fwPackageName(const char* kit, const char* version, char* out, size_t n) {
    std::snprintf(out, n, "%s-%s", kit, version);
}

const char* fwPkgVerdictText(FwPkgVerdict v) {
    switch (v) {
        case FwPkgVerdict::Ok:           return "ok";
        case FwPkgVerdict::NoImage:      return "no CrossPad.bin";
        case FwPkgVerdict::NotAnImage:   return "not a firmware image";
        case FwPkgVerdict::WrongProject: return "another project";
        case FwPkgVerdict::WrongBoard:   return "built for another board revision";
        case FwPkgVerdict::TooLarge:     return "too large for the slot";
        case FwPkgVerdict::UnknownBuild: return "built before packages existed";
        case FwPkgVerdict::Downloading:  return "downloading";
    }
    return "unknown";
}

} // namespace crosspad

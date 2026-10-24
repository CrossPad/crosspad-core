// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace crosspad {

/// Longest firmware version text including the NUL: what a catalog entry, the
/// persisted latest version and the status field all hold.
constexpr size_t kFwVersionMax = 24;

/// A firmware version: major.minor.patch, an optional pre-release ("rc1",
/// "beta.2") and, for a development build named by `git describe`, how many
/// commits it is past that tag ("v1.0.1-284-gd676096" = 1.0.1 + 284 commits).
struct Semver {
    static constexpr size_t kPreMax = 24;
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t patch = 0;
    char pre[kPreMax] = {};
    uint16_t commits = 0;

    /// "1.2.3", "v1.2.3", "1.2" (patch 0), "1.3.0-rc1", "v1.1.0-rc1-64-gc6d227d",
    /// "...-dirty", "...+build" (dot-separated [0-9A-Za-z-] like a pre-release).
    /// False for anything else; `out` is then zeroed.
    static bool parse(const char* text, Semver& out);
};

/// <0, 0, >0. A pre-release sorts below its release; a describe build sorts
/// above the tag it counts from and below the next version or pre-release.
int compareSemver(const Semver& a, const Semver& b);

inline bool operator<(const Semver& a, const Semver& b) { return compareSemver(a, b) < 0; }
inline bool operator==(const Semver& a, const Semver& b) { return compareSemver(a, b) == 0; }

/// Both strings parsed and compared; unparseable text sorts lowest (two
/// unparseable strings compare equal).
int compareVersionText(const char* a, const char* b);

} // namespace crosspad

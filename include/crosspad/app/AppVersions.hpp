// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

namespace crosspad {

/**
 * @brief Build-time inventory of the submodules that went into this binary.
 *
 * The AppRegistry knows app names at runtime but nothing about where they came
 * from, so a running unit cannot otherwise answer "which sampler is this?".
 * Each platform's build system generates the table (walking its app component
 * directories and asking git); this header is the shared shape and accessor so
 * firmware, simulator and tooling all read the same thing.
 *
 * `ref` is the manifest pin when the project records one, otherwise the branch
 * that was checked out. `dirty` means the worktree had uncommitted changes at
 * configure time — the commit alone does not describe that build.
 */
struct AppVersion {
    const char* component;  ///< e.g. "crosspad-sampler"
    const char* appId;      ///< registry id, "" for shared submodules
    const char* commit;     ///< short SHA at configure time
    const char* ref;        ///< manifest pin, else checked-out branch
    bool dirty;             ///< worktree had uncommitted changes
};

/// Number of entries in the generated table.
std::size_t appVersionCount();

/// Entry @p idx, or nullptr when out of range.
const AppVersion* appVersionGet(std::size_t idx);

} // namespace crosspad

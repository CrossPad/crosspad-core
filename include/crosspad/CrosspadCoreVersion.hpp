#pragma once

/**
 * @file CrosspadCoreVersion.hpp
 * @brief Crosspad-core library version
 *
 * Bump rules: MAJOR when an interface a consumer implements or calls changes
 * shape; MINOR when something is added; PATCH for fixes. Consumers gate on
 * these with crosspad/RequireVersion.hpp.
 */

#include <cstdint>

#define CROSSPAD_CORE_VERSION_MAJOR 0
#define CROSSPAD_CORE_VERSION_MINOR 4
#define CROSSPAD_CORE_VERSION_PATCH 0
#define CROSSPAD_CORE_VERSION "0.4.0"

namespace crosspad {

/** A semantic version triple, as reported by a compiled library. */
struct LibVersion {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
};

/** How a compiled library relates to the headers it was built alongside. */
enum class VersionMatch : uint8_t {
    Exact,         ///< identical
    Drift,         ///< same major — minor/patch differ (stale object, untested pairing)
    Incompatible,  ///< major differs
};

/**
 * @brief Version baked into crosspad-core's compiled objects.
 *
 * The CROSSPAD_CORE_VERSION_* macros above describe the headers a consumer
 * compiled against; this describes the library that was linked. They differ
 * when a build reused stale objects or a prebuilt library — the one mismatch
 * the compile-time gate cannot see.
 */
LibVersion coreVersion();

/** Compare a linked library's version against the header triple. */
VersionMatch compareVersion(LibVersion have, uint8_t wantMajor, uint8_t wantMinor, uint8_t wantPatch);

} // namespace crosspad

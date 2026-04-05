// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace crosspad {

/// Resolve a sample file path relative to a kit.json location.
/// If sampleFile starts with '/', returns it as-is (absolute path).
/// Otherwise prepends the kit's directory (extracted from kitJsonPath).
std::string resolveKitSamplePath(const std::string& kitJsonPath,
                                  const std::string& sampleFile);

} // namespace crosspad

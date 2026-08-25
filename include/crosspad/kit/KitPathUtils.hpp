// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace crosspad {

/// Tell the resolver which mount prefix turns a logical kit path into a real
/// filesystem path (e.g. "/sdcard" on ESP32, "" on PC). PortableKitLoader keeps
/// this in sync with its own root prefix; call it directly only if you resolve
/// sample paths without a loader.
void setKitPathFsPrefix(const std::string& prefix);

/// Resolve a sample file path relative to a kit.json location.
/// If sampleFile starts with '/', returns it as-is (absolute path).
/// Otherwise returns the kit-relative location of the sample, handling both
/// layouts in the wild: samples next to kit.json, and samples gathered in a
/// SAMPLES/ subdirectory (how packaged libraries ship). kit.json names the
/// bare file either way, so the on-disk layout decides.
std::string resolveKitSamplePath(const std::string& kitJsonPath,
                                  const std::string& sampleFile);

} // namespace crosspad

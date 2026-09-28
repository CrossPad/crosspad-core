// SPDX-License-Identifier: MIT
#pragma once

#include "crosspad/sampletools/SampleFx.hpp"

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

/// The filesystem path of a logical one -- what resolveKitSamplePath()
/// returns, or a sample picked in the file browser -- for code that opens the
/// file itself rather than handing it to the sample player (which adds the
/// mount on its own). "" stays "".
std::string kitFsPath(const std::string& logicalPath);

/// Where a sample played with @p fx is kept: the sample player can neither
/// transpose nor filter, so such a pad plays a copy rendered once, in a hidden
/// folder beside the sample, a file per setting --
/// "<dir>/.fx/<stem>_p+3_h200_l8000.wav", only what is set named. Logical in,
/// logical out; no fx is the sample itself.
std::string renderedSamplePath(const std::string& logicalPath, const SampleFx& fx);

/// Filesystem path of a file that lives beside a kit's JSON — artwork such as
/// cover.png / thumb.png, or anything else a kit ships next to itself. Unlike
/// the sample resolver this returns the real path (mount prefix included),
/// because the caller hands it straight to a file API or an image widget.
std::string kitAssetPath(const std::string& kitJsonPath,
                         const std::string& fileName);

} // namespace crosspad

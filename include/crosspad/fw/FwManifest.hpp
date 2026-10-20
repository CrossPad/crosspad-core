// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>
#include "crosspad/fw/FwCatalog.hpp"

namespace crosspad {

/// CrossPad.bin, assets.bin and (since core 1.41) icon.bin.
constexpr size_t kFwManifestFilesMax = 3;
constexpr const char* kFwManifestIconName = "icon.bin";

struct FwManifestFile {
    char name[16];      ///< "CrossPad.bin" | "assets.bin" | "icon.bin"
    uint32_t size;
    char sha256[65];    ///< hex, as written by tools/make_fw_package.py
};

/// A package's manifest.json (tools/make_fw_package.py). The signed copy is
/// the authority over the catalog entry that pointed at it.
struct FwManifest {
    char name[48];
    char kit[kFwKitMax];
    char version[32];
    uint8_t boardRev;
    FwChannel channel;
    FwTier tier;
    char tag[kFwTagMax];
    uint8_t fileCount;
    FwManifestFile files[kFwManifestFilesMax];
};

/// A signed manifest is the authority, so a bad entry for any listed file
/// (icon.bin too: size 0, a bad sha, over kFwIconBytesMax) makes the package
/// unparseable, where the unsigned catalog only drops a bad icon.

/// False unless kit, version, board_rev and a CrossPad.bin entry with a
/// 64-hex SHA-256 and a non-zero size are all present and valid. Manifests
/// made before channels existed read as stable / official / no tag. `name` must
/// pass fwSafeName (it may become a directory name). `out` is left untouched
/// on failure. The sha256 may be upper or lower case: compare case-insensitively.
bool parseFwManifest(const char* json, size_t len, FwManifest& out, ArduinoJson::Allocator* alloc = nullptr);
const FwManifestFile* fwManifestFile(const FwManifest& m, const char* name);
/// Same kit, same board revision, and the same version as the catalog said.
bool fwManifestMatches(const FwManifest& m, const char* kit, const char* catalogVersion, uint8_t boardRev);

} // namespace crosspad

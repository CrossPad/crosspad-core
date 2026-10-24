// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>
#include "crosspad/fw/FwKit.hpp"
#include "crosspad/fw/Semver.hpp"

namespace crosspad {

constexpr size_t kFwTagMax = 32;
/// A release asset is CrossPad-<tag>-v<rev>-<kit><suffix>. The device never
/// builds the name: it takes it from the catalog of its own board revision.
constexpr size_t kFwAssetNameMax = 80;
/// The widest revision is "v255" (CPFI's board_rev is one byte), the longest suffix "-manifest.json".
static_assert(sizeof "CrossPad-" - 1 + (kFwTagMax - 1) + sizeof "-v255-" - 1 + (kFwKitMax - 1) +
                      sizeof "-manifest.json" <= kFwAssetNameMax,
              "the longest release asset name fits, NUL included");
constexpr size_t kFwCatalogKits = 16;
constexpr size_t kFwCatalogVersions = 8;
/// A kit icon is a 48x48 LVGL image (~7 kB); anything larger is not one.
constexpr uint32_t kFwIconBytesMax = 16u * 1024u;
/// The one icon file the device draws: an LVGL v9 binary image, RGB565A8,
/// 48x48 (tools/fw_icon.py) -- its 12-byte header, RGB565, then the alpha plane.
constexpr uint32_t kFwIconSide = 48;
constexpr size_t kFwIconHeaderBytes = 12;
constexpr size_t kFwIconBytes = kFwIconHeaderBytes + kFwIconSide * kFwIconSide * 3;

enum class FwChannel : uint8_t { Stable, Beta };
enum class FwTier : uint8_t { Official, Community };

struct FwCatalogFile {
    char name[kFwAssetNameMax];
    uint32_t size;
};

struct FwCatalogVersion {
    char version[kFwVersionMax];
    char tag[kFwTagMax];
    FwChannel channel;
    FwTier tier;
    FwCatalogFile bin;
    FwCatalogFile assets;
    FwCatalogFile icon;   ///< 48x48 LVGL image of the kit; name "" and size 0 when the release has none
    char manifest[kFwAssetNameMax];
    char sig[kFwAssetNameMax];
};

struct FwCatalogKit {
    char kit[kFwKitMax];
    uint8_t count;
    FwCatalogVersion versions[kFwCatalogVersions];
};

/// crosspad.app's /api/device/catalog for one board revision. ~61 kB: the
/// platform keeps it in PSRAM and only while it needs it, never on a task stack.
struct FwCatalog {
    uint8_t kitCount;
    FwCatalogKit kits[kFwCatalogKits];
};

/// A name that may go into a URL path or a directory name unescaped: letters,
/// digits and . _ - + only, shorter than `cap`, and not starting with '.'
/// (so never "." or ".."; a hidden or relative name is never a CI asset).
bool fwSafeName(const char* s, size_t cap);

/// A firmware version from a catalog, a manifest or a host: a Semver that is
/// also an fwSafeName shorter than kFwVersionMax. It is stored in NVS, printed
/// in verb lines and shown on the LCD, so nothing else may get that far.
bool fwVersionValid(const char* s);

/// One catalog version entry, validated the one way every reader of the
/// catalog shares (parseFwCatalog and the Store's card cache). False for a
/// version the device cannot use; then `out` is untouched. An icon that is
/// malformed or over kFwIconBytesMax costs the icon (name "", size 0), never
/// the version.
bool fwReadCatalogVersion(FwCatalogVersion& out, JsonVariantConst v);


/// False on anything but {"v":1,"kits":[...]}; then out.kitCount is 0.
/// Entries with a name the device would have to escape, an unknown channel or
/// tier, a zero size or a field past its limit are skipped, as is a kit with
/// no usable version or one already listed; a version already listed for its
/// kit (compared as a version) is skipped, the first wins. Caps at 16 kits x 8 versions.
/// `alloc` lets a platform keep the parse out of scarce RAM.
bool parseFwCatalog(const char* json, size_t len, FwCatalog& out, ArduinoJson::Allocator* alloc = nullptr);

using FwVersionSkip = bool (*)(const char* kit, const char* version, void* ctx);

/// The newest version of `kit` strictly newer than `running`; Stable sees
/// stable versions only, Beta sees both. nullptr when there is none.
const FwCatalogVersion* fwCatalogPickUpdate(const FwCatalog& c, const char* kit, FwChannel channel,
                                            const Semver& running, FwVersionSkip skip = nullptr,
                                            void* ctx = nullptr);

/// `version` compared as a version ("v1.3.0" finds "1.3.0").
const FwCatalogVersion* fwCatalogFind(const FwCatalog& c, const char* kit, const char* version);

/// The update server's base URL ("http(s)://host[:port]"), bytes incl. NUL.
constexpr size_t kFwBaseUrlMax = 128;
constexpr const char* kFwFileApi = "/api/device/file/";
/// <base>/api/device/file/<tag>/<name> with every part at its limit, incl. NUL.
constexpr size_t kFwFileUrlMax = kFwBaseUrlMax + sizeof("/api/device/file/") + kFwTagMax + 1 + kFwAssetNameMax;

/// <baseUrl>/api/device/file/<tag>/<name>; tag and name were checked to be
/// URL-safe by the catalog parser, so nothing is escaped. False when it did
/// not fit `n`: a cut-off URL names another file.
bool fwCatalogFileUrl(const char* baseUrl, const char* tag, const char* name, char* out, size_t n);

} // namespace crosspad

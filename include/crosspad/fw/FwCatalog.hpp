#pragma once
#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>
#include "crosspad/fw/FwKit.hpp"
#include "crosspad/fw/Semver.hpp"

namespace crosspad {

constexpr size_t kFwVersionMax = 24;
constexpr size_t kFwTagMax = 32;
/// "CrossPad-" + tag (31) + "-v2-" + kit (15) + "-manifest.json" + NUL
constexpr size_t kFwAssetNameMax = 80;
constexpr size_t kFwCatalogKits = 16;
constexpr size_t kFwCatalogVersions = 8;

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
    char manifest[kFwAssetNameMax];
    char sig[kFwAssetNameMax];
};

struct FwCatalogKit {
    char kit[kFwKitMax];
    uint8_t count;
    FwCatalogVersion versions[kFwCatalogVersions];
};

/// crosspad.app's /api/device/catalog for one board revision. ~50 kB: the
/// platform keeps it in PSRAM and only while it needs it.
struct FwCatalog {
    uint8_t kitCount;
    FwCatalogKit kits[kFwCatalogKits];
};

/// A name that may go into a URL path or a directory name unescaped: letters,
/// digits and . _ - + only, shorter than `cap`, and not starting with '.'
/// (so never "." or ".."; a hidden or relative name is never a CI asset).
bool fwSafeName(const char* s, size_t cap);

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

} // namespace crosspad

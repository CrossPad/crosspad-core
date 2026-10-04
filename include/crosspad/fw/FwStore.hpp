#pragma once
#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>
#include "crosspad/fw/FwCatalog.hpp"
#include "crosspad/fw/Semver.hpp"

namespace crosspad {

/// Device buffers for the text CI validates (tools/ci_matrix.py): bytes incl. NUL.
constexpr size_t kFwDisplayNameMax = 32;
constexpr size_t kFwAuthorMax = 32;
constexpr size_t kFwDescriptionMax = 192;
constexpr size_t kFwNotesMax = 400;
constexpr size_t kFwAppIdMax = 24;
constexpr size_t kFwAppsMax = 16;
/// A version as an image or a package names it ("v1.1.0-rc1-64-gc6d227d").
constexpr size_t kFwOwnedVersionMax = 32;

/// At most cap-1 bytes of `text`, cut before a UTF-8 sequence rather than
/// through it, always NUL-terminated. nullptr copies "". Display text only:
/// it is never a path, a URL part or a directory name.
void fwCopyText(char* out, size_t cap, const char* text);

struct FwStoreVersion {
    char version[kFwVersionMax];
    FwChannel channel;
    bool hasIcon;
    uint32_t bytes;   ///< what a download puts on the card: image + assets + icon
};

/// One kit of the card cache. display_name / author are the catalog's per-kit fields.
struct FwStoreKit {
    char kit[kFwKitMax];
    char displayName[kFwDisplayNameMax];   ///< the kit id when the catalog has none
    char author[kFwAuthorMax];
    FwTier tier;
    uint8_t count;
    FwStoreVersion versions[kFwCatalogVersions];
};

/// What the Store and Updates views show (~5.4 kB): the catalog cached on the
/// card by the last network-mode check.
struct FwStore {
    int64_t fetched;   ///< server time of that check, 0 unknown
    uint8_t kitCount;
    FwStoreKit kits[kFwCatalogKits];
};

/// Missing is the platform's (no file); the parser returns the other three.
enum class FwStoreLoad : uint8_t { Ok, Missing, Format, OtherBoard };
const char* fwStoreLoadName(FwStoreLoad r);   ///< "ok" "missing" "format" "other_board"

/// Fills `filter` with the fields the Store needs, so a stream parse keeps
/// ~20 kB of a cache that can be 160 kB.
void fwStoreCacheFilter(JsonDocument& filter);
FwStoreLoad fwStoreFromDoc(const JsonDocument& doc, uint8_t boardRev, FwStore& out);

/// The card cache {"v":1,"fetched":<unix>,"rev":"v<N>","catalog":{...}}.
/// Format for a broken or other-version envelope, OtherBoard for another
/// revision's catalog; out.kitCount is 0 unless Ok. Kits and versions the
/// device cannot use are skipped, as parseFwCatalog skips them (the first of
/// two equal versions wins). `alloc` lets a platform keep the parse out of scarce RAM.
FwStoreLoad parseFwStoreCache(const char* json, size_t len, uint8_t boardRev, FwStore& out,
                              ArduinoJson::Allocator* alloc = nullptr);

/// The same from a stream (an ArduinoJson reader: int read(), size_t readBytes(char*, size_t)).
template <class Reader>
FwStoreLoad parseFwStoreCacheFrom(Reader& reader, uint8_t boardRev, FwStore& out,
                                  ArduinoJson::Allocator* alloc = nullptr) {
    JsonDocument filter = alloc ? JsonDocument(alloc) : JsonDocument();
    fwStoreCacheFilter(filter);
    JsonDocument doc = alloc ? JsonDocument(alloc) : JsonDocument();
    if (deserializeJson(doc, reader, DeserializationOption::Filter(filter))) {
        out.kitCount = 0;
        return FwStoreLoad::Format;
    }
    return fwStoreFromDoc(doc, boardRev, out);
}

const FwStoreKit* fwStoreFind(const FwStore& s, const char* kit);
/// Index of `version` in the kit ("v1.3.0" finds "1.3.0"), -1 when absent.
int fwStoreVersionIndex(const FwStoreKit& k, const char* version);

/// The version a Store tile offers: the newest in `channel` (Beta sees every
/// version), and for a kit with no stable version its newest beta, so a
/// beta-only kit is still visible. -1 for a kit without versions. Works on
/// FwStoreKit and on FwCatalogKit (B's job fetches the same icons).
template <class Kit>
int fwShownVersion(const Kit& k, FwChannel channel) {
    int best = -1;
    int newest = -1;
    Semver bestV{};
    Semver newestV{};
    for (int i = 0; i < static_cast<int>(k.count); ++i) {
        Semver v;
        if (!Semver::parse(k.versions[i].version, v)) continue;
        if (newest < 0 || newestV < v) {
            newest = i;
            newestV = v;
        }
        if (channel == FwChannel::Stable && k.versions[i].channel != FwChannel::Stable) continue;
        if (best < 0 || bestV < v) {
            best = i;
            bestV = v;
        }
    }
    return best >= 0 ? best : newest;
}

/// Store order: official kits, then community, each by display name
/// (ASCII case-insensitive), then kit id. Indices into s.kits; returns how many.
size_t fwStoreOrder(const FwStore& s, uint8_t* order, size_t cap, size_t* firstCommunity);

/// The "Older versions" list: every version below the kit's newest (any
/// channel), newest first. Indices into k.versions.
size_t fwStoreOlder(const FwStoreKit& k, uint8_t* out, size_t cap);

using FwKitIsBeta = bool (*)(const char* kit, void* ctx);

/// A kit the card or a slot holds, at a version.
struct FwOwned {
    char kit[kFwKitMax];
    char version[kFwOwnedVersionMax];
};

struct FwUpdateRow {
    uint8_t kit;       ///< index into FwStore::kits
    uint8_t version;   ///< index into that kit's versions: the update
    char have[kFwOwnedVersionMax];   ///< the newest version owned, as given
};

/// One row per owned kit with a newer version in its channel than the newest
/// owned one (the same kit may be owned several times), in fwStoreOrder().
size_t fwStoreUpdates(const FwStore& s, const FwOwned* owned, size_t n, FwKitIsBeta isBeta, void* ctx,
                      FwUpdateRow* out, size_t cap);

/// A kit card's text. Notes come only from the catalog's kit endpoint (written after CI, not signed).
/// Every field is display text cut to its buffer; app ids are fwSafeName-clean.
struct FwKitDetail {
    char displayName[kFwDisplayNameMax];
    char author[kFwAuthorMax];
    char description[kFwDescriptionMax];
    char notes[kFwNotesMax];
    uint8_t appCount;
    char apps[kFwAppsMax][kFwAppIdMax];
    FwTier tier;
};

bool fwKitDetailFromDoc(const JsonDocument& doc, const char* version, FwKitDetail& out);

/// From .details/<kit>.json (/api/device/kit/<kit> verbatim): false when
/// `version` is not in it; `out` is then untouched.
bool parseFwKitDetail(const char* kitJson, size_t len, const char* version, FwKitDetail& out,
                      ArduinoJson::Allocator* alloc = nullptr);

template <class Reader>
bool parseFwKitDetailFrom(Reader& reader, const char* version, FwKitDetail& out,
                          ArduinoJson::Allocator* alloc = nullptr) {
    JsonDocument doc = alloc ? JsonDocument(alloc) : JsonDocument();
    if (deserializeJson(doc, reader)) return false;
    return fwKitDetailFromDoc(doc, version, out);
}

/// From a package's manifest.json, display fields only (a hand-made package
/// without files still has a name to show). False only for broken JSON,
/// `out` is then untouched.
bool parseFwManifestDetail(const char* json, size_t len, FwKitDetail& out, ArduinoJson::Allocator* alloc = nullptr);

/// Copies into `into` every field it left empty (the signed manifest wins, the catalog fills in).
void fwKitDetailMerge(FwKitDetail& into, const FwKitDetail& from);

} // namespace crosspad

#include "crosspad/fw/FwStore.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace crosspad {
namespace {

constexpr int kCacheVersion = 1;
constexpr int kCatalogVersion = 1;
constexpr unsigned kRevMax = 255;
constexpr unsigned char kUtf8ContinuationMask = 0xC0;
constexpr unsigned char kUtf8Continuation = 0x80;
constexpr unsigned char kUtf8ContinuationLast = 0xBF;
constexpr unsigned char kAsciiLimit = 0x80;
constexpr unsigned char kSpace = 0x20;
constexpr unsigned char kDel = 0x7F;

// Length of the well-formed UTF-8 sequence at `p` (lead byte >= 0x80), 0 when
// it is not one: stray continuation, 0xC0/0xC1 and 0xF5.. leads, overlong
// forms, surrogates, past U+10FFFF, or cut short by the end of the string.
size_t utf8SequenceLength(const char* p) {
    const unsigned char lead = static_cast<unsigned char>(p[0]);
    size_t len;
    unsigned char lo = kUtf8Continuation;   // allowed range of the second byte
    unsigned char hi = kUtf8ContinuationLast;
    if (lead >= 0xC2 && lead <= 0xDF) len = 2;
    else if (lead >= 0xE0 && lead <= 0xEF) {
        len = 3;
        if (lead == 0xE0) lo = 0xA0;   // else overlong
        if (lead == 0xED) hi = 0x9F;   // else a surrogate
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        len = 4;
        if (lead == 0xF0) lo = 0x90;   // else overlong
        if (lead == 0xF4) hi = 0x8F;   // else past U+10FFFF
    } else return 0;
    for (size_t i = 1; i < len; ++i) {
        const unsigned char b = static_cast<unsigned char>(p[i]);   // a NUL fails the range test
        if (b < (i == 1 ? lo : kUtf8Continuation) || b > (i == 1 ? hi : kUtf8ContinuationLast)) return 0;
    }
    return len;
}

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

int compareNoCase(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const char ca = lowerAscii(*a);
        const char cb = lowerAscii(*b);
        if (ca != cb || !ca) return static_cast<unsigned char>(ca) - static_cast<unsigned char>(cb);
    }
}

bool parseRev(const char* s, uint8_t& rev) {   // "v2" -> 2
    if (!s || s[0] != 'v' || !s[1]) return false;
    unsigned n = 0;
    for (const char* p = s + 1; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        n = n * 10 + static_cast<unsigned>(*p - '0');
        if (n > kRevMax) return false;
    }
    rev = static_cast<uint8_t>(n);
    return true;
}

bool readTier(const char* s, FwTier& out) {
    if (std::strcmp(s, "official") == 0) out = FwTier::Official;
    else if (std::strcmp(s, "community") == 0) out = FwTier::Community;
    else return false;
    return true;
}

bool hasApp(const FwKitDetail& d, const char* id) {
    for (size_t i = 0; i < d.appCount; ++i)
        if (std::strcmp(d.apps[i], id) == 0) return true;
    return false;
}

void readDetail(JsonVariantConst v, FwKitDetail& out) {
    std::memset(&out, 0, sizeof out);   // no ~1 kB temporary on a job stack
    fwCopyLine(out.displayName, sizeof out.displayName, v["display_name"] | "");
    fwCopyLine(out.author, sizeof out.author, v["author"] | "");
    fwCopyText(out.description, sizeof out.description, v["description"] | "");
    fwCopyText(out.notes, sizeof out.notes, v["notes"] | "");
    for (JsonVariantConst a : v["apps"].as<JsonArrayConst>()) {
        if (out.appCount >= kFwAppsMax) break;
        const char* id = a.as<const char*>();
        if (!fwSafeName(id, kFwAppIdMax) || hasApp(out, id)) continue;
        std::memcpy(out.apps[out.appCount++], id, std::strlen(id) + 1);
    }
    FwTier tier = FwTier::Official;
    readTier(v["tier"] | "official", tier);
    out.tier = tier;
}

uint32_t saturatingSum(uint32_t a, uint32_t b) {
    return b > UINT32_MAX - a ? UINT32_MAX : a + b;
}

bool hasVersion(const FwStoreKit& k, const Semver& want) {
    for (size_t i = 0; i < k.count; ++i) {
        Semver have;
        if (Semver::parse(k.versions[i].version, have) && have == want) return true;
    }
    return false;
}

bool isOlder(const FwStoreKit& k, uint8_t a, uint8_t b) {
    return compareVersionText(k.versions[a].version, k.versions[b].version) < 0;
}

void copyText(char* out, size_t cap, const char* text, bool keepNewline) {
    if (cap == 0) return;
    size_t n = 0;
    // One pass over the input: a valid sequence is copied whole or not at all,
    // an invalid one (and the continuation bytes trailing it) becomes one '?',
    // and control characters (and '\n' unless keepNewline) are dropped. This text is
    // written by people after CI and ends up in LVGL labels.
    for (const char* p = text; p && *p;) {
        const unsigned char c = static_cast<unsigned char>(*p);
        size_t len = 1;
        bool valid = true;
        if (c < kAsciiLimit) {
            if ((c < kSpace && !(keepNewline && c == '\n')) || c == kDel) {
                ++p;
                continue;
            }
        } else {
            len = utf8SequenceLength(p);
            valid = len != 0;
        }
        if (!valid) {
            ++p;
            while ((static_cast<unsigned char>(*p) & kUtf8ContinuationMask) == kUtf8Continuation) ++p;
            len = 1;
        }
        if (n + len > cap - 1) break;
        if (valid) {
            std::memcpy(out + n, p, len);
            p += len;
        } else {
            out[n] = '?';
        }
        n += len;
    }
    out[n] = '\0';
}

} // namespace

void fwCopyText(char* out, size_t cap, const char* text) { copyText(out, cap, text, true); }

void fwCopyLine(char* out, size_t cap, const char* text) { copyText(out, cap, text, false); }

const char* fwStoreLoadName(FwStoreLoad r) {
    switch (r) {
        case FwStoreLoad::Ok:         return "ok";
        case FwStoreLoad::Missing:    return "missing";
        case FwStoreLoad::Format:     return "format";
        case FwStoreLoad::OtherBoard: return "other_board";
    }
    return "?";
}

void fwStoreCacheFilter(JsonDocument& f) {
    f["v"] = true;
    f["fetched"] = true;
    f["rev"] = true;
    f["catalog"]["v"] = true;
    // Whole chains: a proxy converted to a JsonVariant does not create the
    // element, so each path is written out from the root.
    f["catalog"]["kits"][0]["kit"] = true;
    f["catalog"]["kits"][0]["display_name"] = true;
    f["catalog"]["kits"][0]["author"] = true;
    f["catalog"]["kits"][0]["versions"][0]["version"] = true;
    f["catalog"]["kits"][0]["versions"][0]["channel"] = true;
    f["catalog"]["kits"][0]["versions"][0]["tier"] = true;
    f["catalog"]["kits"][0]["versions"][0]["tag"] = true;
    f["catalog"]["kits"][0]["versions"][0]["manifest"] = true;
    f["catalog"]["kits"][0]["versions"][0]["sig"] = true;
    f["catalog"]["kits"][0]["versions"][0]["bin"]["name"] = true;
    f["catalog"]["kits"][0]["versions"][0]["bin"]["size"] = true;
    f["catalog"]["kits"][0]["versions"][0]["assets"]["name"] = true;
    f["catalog"]["kits"][0]["versions"][0]["assets"]["size"] = true;
    f["catalog"]["kits"][0]["versions"][0]["icon"]["name"] = true;
    f["catalog"]["kits"][0]["versions"][0]["icon"]["size"] = true;
}

FwStoreLoad fwStoreFromDoc(const JsonDocument& doc, uint8_t boardRev, FwStore& out) {
    out.kitCount = 0;
    out.fetched = 0;
    if ((doc["v"] | 0) != kCacheVersion || (doc["catalog"]["v"] | 0) != kCatalogVersion) return FwStoreLoad::Format;
    uint8_t rev = 0;
    if (!parseRev(doc["rev"] | "", rev)) return FwStoreLoad::Format;
    if (rev != boardRev) return FwStoreLoad::OtherBoard;
    JsonArrayConst kits = doc["catalog"]["kits"].as<JsonArrayConst>();
    if (kits.isNull()) return FwStoreLoad::Format;
    out.fetched = std::max<int64_t>(doc["fetched"].as<int64_t>(), 0);
    for (JsonVariantConst k : kits) {
        if (out.kitCount >= kFwCatalogKits) break;
        const char* kit = k["kit"] | "";
        if (!fwKitIdValid(kit) || fwStoreFind(out, kit)) continue;
        FwStoreKit& dst = out.kits[out.kitCount];
        std::memset(&dst, 0, sizeof dst);
        std::memcpy(dst.kit, kit, std::strlen(kit) + 1);
        bool any = false;
        Semver newestV{};
        for (JsonVariantConst v : k["versions"].as<JsonArrayConst>()) {
            if (dst.count >= kFwCatalogVersions) break;
            FwCatalogVersion cand;   // ~450 B: the same validator parseFwCatalog uses
            Semver parsed;
            if (!fwReadCatalogVersion(cand, v) || !Semver::parse(cand.version, parsed) || hasVersion(dst, parsed))
                continue;
            FwStoreVersion& sv = dst.versions[dst.count++];
            std::memcpy(sv.version, cand.version, sizeof sv.version);
            sv.channel = cand.channel;
            sv.tier = cand.tier;
            sv.hasIcon = cand.icon.size > 0;
            sv.bytes = saturatingSum(saturatingSum(cand.bin.size, cand.assets.size), cand.icon.size);
            if (!any || newestV < parsed) {   // the tile's tier is the newest version's
                any = true;
                newestV = parsed;
                dst.tier = cand.tier;
            }
        }
        if (dst.count == 0) continue;
        fwCopyLine(dst.displayName, sizeof dst.displayName, k["display_name"] | "");
        if (!dst.displayName[0]) fwCopyLine(dst.displayName, sizeof dst.displayName, kit);
        fwCopyLine(dst.author, sizeof dst.author, k["author"] | "");
        ++out.kitCount;
    }
    return FwStoreLoad::Ok;
}

FwStoreLoad parseFwStoreCache(const char* json, size_t len, uint8_t boardRev, FwStore& out,
                              ArduinoJson::Allocator* alloc) {
    out.kitCount = 0;
    out.fetched = 0;
    if (!json || len == 0) return FwStoreLoad::Format;
    JsonDocument filter = alloc ? JsonDocument(alloc) : JsonDocument();
    fwStoreCacheFilter(filter);
    JsonDocument doc = alloc ? JsonDocument(alloc) : JsonDocument();
    if (deserializeJson(doc, json, len, DeserializationOption::Filter(filter))) return FwStoreLoad::Format;
    return fwStoreFromDoc(doc, boardRev, out);
}

const FwStoreKit* fwStoreFind(const FwStore& s, const char* kit) {
    for (size_t i = 0; i < s.kitCount; ++i)
        if (std::strcmp(s.kits[i].kit, kit) == 0) return &s.kits[i];
    return nullptr;
}

int fwStoreVersionIndex(const FwStoreKit& k, const char* version) {
    Semver want;
    if (!version || !Semver::parse(version, want)) return -1;
    for (int i = 0; i < static_cast<int>(k.count); ++i) {
        Semver have;
        if (Semver::parse(k.versions[i].version, have) && have == want) return i;
    }
    return -1;
}

size_t fwStoreOrder(const FwStore& s, uint8_t* order, size_t cap, size_t* firstCommunity) {
    size_t n = 0;
    for (size_t i = 0; i < s.kitCount && n < cap; ++i) order[n++] = static_cast<uint8_t>(i);
    auto before = [&](uint8_t a, uint8_t b) {
        const FwStoreKit& ka = s.kits[a];
        const FwStoreKit& kb = s.kits[b];
        if (ka.tier != kb.tier) return ka.tier == FwTier::Official;
        const int byName = compareNoCase(ka.displayName, kb.displayName);
        if (byName != 0) return byName < 0;
        return std::strcmp(ka.kit, kb.kit) < 0;
    };
    for (size_t i = 1; i < n; ++i) {   // insertion sort: 16 entries at most, and stable
        const uint8_t v = order[i];
        size_t j = i;
        for (; j > 0 && before(v, order[j - 1]); --j) order[j] = order[j - 1];
        order[j] = v;
    }
    size_t first = n;
    for (size_t i = 0; i < n; ++i) {
        if (s.kits[order[i]].tier == FwTier::Community) {
            first = i;
            break;
        }
    }
    if (firstCommunity) *firstCommunity = first;
    return n;
}

size_t fwStoreOlder(const FwStoreKit& k, uint8_t* out, size_t cap) {
    const int newest = fwShownVersion(k, FwChannel::Beta);
    if (newest < 0) return 0;
    size_t n = 0;
    for (int i = 0; i < static_cast<int>(k.count) && n < cap; ++i) {
        if (i == newest) continue;
        if (compareVersionText(k.versions[i].version, k.versions[newest].version) >= 0) continue;
        out[n++] = static_cast<uint8_t>(i);
    }
    for (size_t i = 1; i < n; ++i) {
        const uint8_t v = out[i];
        size_t j = i;
        for (; j > 0 && isOlder(k, out[j - 1], v); --j) out[j] = out[j - 1];
        out[j] = v;
    }
    return n;
}

size_t fwStoreUpdates(const FwStore& s, const FwOwned* owned, size_t n, FwKitIsBeta isBeta, void* ctx,
                      FwUpdateRow* out, size_t cap) {
    uint8_t order[kFwCatalogKits];
    const size_t kits = fwStoreOrder(s, order, kFwCatalogKits, nullptr);
    size_t rows = 0;
    for (size_t o = 0; o < kits && rows < cap; ++o) {
        const FwStoreKit& k = s.kits[order[o]];
        const FwOwned* have = nullptr;
        Semver haveV{};
        for (size_t i = 0; i < n; ++i) {
            Semver v;
            if (std::strcmp(owned[i].kit, k.kit) != 0 || !Semver::parse(owned[i].version, v)) continue;
            if (!have || haveV < v) {
                have = &owned[i];
                haveV = v;
            }
        }
        if (!have) continue;
        const bool beta = isBeta && isBeta(k.kit, ctx);
        int best = -1;
        Semver bestV{};
        for (int i = 0; i < static_cast<int>(k.count); ++i) {
            if (!beta && k.versions[i].channel != FwChannel::Stable) continue;
            Semver v;
            if (!Semver::parse(k.versions[i].version, v) || !(haveV < v)) continue;
            if (best < 0 || bestV < v) {
                best = i;
                bestV = v;
            }
        }
        if (best < 0) continue;
        FwUpdateRow& r = out[rows++];
        r.kit = order[o];
        r.version = static_cast<uint8_t>(best);
        fwCopyLine(r.have, sizeof r.have, have->version);
    }
    return rows;
}

bool fwKitDetailFromDoc(const JsonDocument& doc, const char* version, FwKitDetail& out) {
    Semver want;
    if (!version || !Semver::parse(version, want)) return false;
    for (JsonVariantConst v : doc["versions"].as<JsonArrayConst>()) {
        Semver have;
        if (!Semver::parse(v["version"] | "", have) || !(have == want)) continue;
        readDetail(v, out);
        return true;
    }
    return false;
}

bool parseFwKitDetail(const char* kitJson, size_t len, const char* version, FwKitDetail& out,
                      ArduinoJson::Allocator* alloc) {
    if (!kitJson || len == 0) return false;
    JsonDocument doc = alloc ? JsonDocument(alloc) : JsonDocument();
    if (deserializeJson(doc, kitJson, len)) return false;
    return fwKitDetailFromDoc(doc, version, out);
}

bool parseFwManifestDetail(const char* json, size_t len, FwKitDetail& out, ArduinoJson::Allocator* alloc) {
    if (!json || len == 0) return false;
    JsonDocument doc = alloc ? JsonDocument(alloc) : JsonDocument();
    if (deserializeJson(doc, json, len)) return false;
    readDetail(doc.as<JsonVariantConst>(), out);
    out.notes[0] = '\0';
    return true;
}

void fwKitDetailMerge(FwKitDetail& into, const FwKitDetail& from) {
    if (!into.displayName[0]) std::memcpy(into.displayName, from.displayName, sizeof into.displayName);
    if (!into.author[0]) std::memcpy(into.author, from.author, sizeof into.author);
    if (!into.description[0]) std::memcpy(into.description, from.description, sizeof into.description);
    if (!into.notes[0]) std::memcpy(into.notes, from.notes, sizeof into.notes);
    if (into.appCount == 0) {
        into.appCount = from.appCount;
        std::memcpy(into.apps, from.apps, sizeof into.apps);
    }
}

} // namespace crosspad

#include "crosspad/fw/FwCatalog.hpp"
#include <cstring>

namespace crosspad {

bool fwSafeName(const char* s, size_t cap) {
    if (!s || !*s || *s == '.') return false;
    size_t n = 0;
    for (const char* p = s; *p; ++p, ++n) {
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '-' || c == '+';
        if (!ok) return false;
    }
    return n < cap;
}

bool fwVersionValid(const char* s) {
    Semver parsed;
    return fwSafeName(s, kFwVersionMax) && Semver::parse(s, parsed);
}

namespace {

bool copyName(char* out, size_t cap, JsonVariantConst v) {
    const char* s = v.as<const char*>();
    if (!fwSafeName(s, cap)) return false;
    std::memcpy(out, s, std::strlen(s) + 1);
    return true;
}

bool readFile(FwCatalogFile& f, JsonVariantConst v) {
    if (!copyName(f.name, sizeof f.name, v["name"])) return false;
    f.size = v["size"] | 0u;
    return f.size > 0;
}

const FwCatalogKit* findKit(const FwCatalog& c, const char* kit) {
    for (size_t i = 0; i < c.kitCount; ++i)
        if (std::strcmp(c.kits[i].kit, kit) == 0) return &c.kits[i];
    return nullptr;
}

bool hasVersion(const FwCatalogKit& k, const char* version) {
    Semver want;
    Semver::parse(version, want);   // fwReadCatalogVersion already accepted it
    for (size_t i = 0; i < k.count; ++i) {
        Semver have;
        if (Semver::parse(k.versions[i].version, have) && have == want) return true;
    }
    return false;
}

} // namespace

bool fwReadCatalogVersion(FwCatalogVersion& out, JsonVariantConst v) {
    FwCatalogVersion tmp{};
    const char* version = v["version"] | "";
    if (!fwVersionValid(version)) return false;
    std::memcpy(tmp.version, version, std::strlen(version) + 1);
    if (!copyName(tmp.tag, sizeof tmp.tag, v["tag"])) return false;
    const char* channel = v["channel"] | "";
    if (std::strcmp(channel, "stable") == 0) tmp.channel = FwChannel::Stable;
    else if (std::strcmp(channel, "beta") == 0) tmp.channel = FwChannel::Beta;
    else return false;
    const char* tier = v["tier"] | "";
    if (std::strcmp(tier, "official") == 0) tmp.tier = FwTier::Official;
    else if (std::strcmp(tier, "community") == 0) tmp.tier = FwTier::Community;
    else return false;
    if (!readFile(tmp.bin, v["bin"]) || !readFile(tmp.assets, v["assets"])) return false;
    // Optional and cosmetic: a bad icon entry costs the icon, never the version.
    JsonVariantConst icon = v["icon"];
    if (!icon.isNull() && (!readFile(tmp.icon, icon) || tmp.icon.size > kFwIconBytesMax)) tmp.icon = FwCatalogFile{};
    if (!copyName(tmp.manifest, sizeof tmp.manifest, v["manifest"]) ||
        !copyName(tmp.sig, sizeof tmp.sig, v["sig"]))
        return false;
    out = tmp;
    return true;
}

bool parseFwCatalog(const char* json, size_t len, FwCatalog& out, ArduinoJson::Allocator* alloc) {
    out.kitCount = 0;
    if (!json || len == 0) return false;
    JsonDocument doc = alloc ? JsonDocument(alloc) : JsonDocument();
    if (deserializeJson(doc, json, len)) return false;
    if ((doc["v"] | 0) != 1) return false;
    JsonArrayConst kits = doc["kits"].as<JsonArrayConst>();
    if (kits.isNull()) return false;
    for (JsonVariantConst k : kits) {
        if (out.kitCount >= kFwCatalogKits) break;
        const char* kit = k["kit"] | "";
        if (!fwKitIdValid(kit) || findKit(out, kit)) continue;
        FwCatalogKit& dst = out.kits[out.kitCount];
        std::memset(&dst, 0, sizeof dst);   // not a temporary: 3 kB is too much for a task stack
        std::memcpy(dst.kit, kit, std::strlen(kit) + 1);
        for (JsonVariantConst v : k["versions"].as<JsonArrayConst>()) {
            if (dst.count >= kFwCatalogVersions) break;
            FwCatalogVersion cand;
            if (!fwReadCatalogVersion(cand, v) || hasVersion(dst, cand.version)) continue;
            dst.versions[dst.count++] = cand;
        }
        if (dst.count > 0) ++out.kitCount;
    }
    return true;
}

const FwCatalogVersion* fwCatalogPickUpdate(const FwCatalog& c, const char* kit, FwChannel channel,
                                            const Semver& running, FwVersionSkip skip, void* ctx) {
    const FwCatalogKit* k = findKit(c, kit);
    if (!k) return nullptr;
    const FwCatalogVersion* best = nullptr;
    Semver bestVersion;
    for (size_t i = 0; i < k->count; ++i) {
        const FwCatalogVersion& v = k->versions[i];
        if (channel == FwChannel::Stable && v.channel != FwChannel::Stable) continue;
        Semver parsed;
        if (!Semver::parse(v.version, parsed) || !(running < parsed)) continue;
        if (skip && skip(kit, v.version, ctx)) continue;
        if (!best || bestVersion < parsed) {
            best = &v;
            bestVersion = parsed;
        }
    }
    return best;
}

const FwCatalogVersion* fwCatalogFind(const FwCatalog& c, const char* kit, const char* version) {
    const FwCatalogKit* k = findKit(c, kit);
    Semver want;
    if (!k || !Semver::parse(version, want)) return nullptr;
    for (size_t i = 0; i < k->count; ++i) {
        Semver have;
        if (Semver::parse(k->versions[i].version, have) && have == want) return &k->versions[i];
    }
    return nullptr;
}

} // namespace crosspad

#include "crosspad/fw/FwManifest.hpp"
#include <cstring>

namespace crosspad {
namespace {

constexpr size_t kShaHexLen = 64;
constexpr int kBoardRevMax = 255;

bool isHex64(const char* s) {
    if (std::strlen(s) != kShaHexLen) return false;
    for (const char* p = s; *p; ++p) {
        const char c = *p;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

bool parseInto(const char* json, size_t len, FwManifest& out, ArduinoJson::Allocator* alloc) {
    std::memset(&out, 0, sizeof out);
    if (!json || len == 0) return false;
    JsonDocument doc = alloc ? JsonDocument(alloc) : JsonDocument();
    if (deserializeJson(doc, json, len)) return false;
    const char* name = doc["name"] | "";
    const char* kit = doc["kit"] | "";
    const char* version = doc["version"] | "";
    const int rev = doc["board_rev"] | -1;
    Semver parsed;
    if (!fwSafeName(name, sizeof out.name) || !fwKitIdValid(kit) || std::strlen(version) >= sizeof out.version ||
        !Semver::parse(version, parsed) || rev < 1 || rev > kBoardRevMax)
        return false;
    const char* channel = doc["channel"] | "stable";
    const char* tier = doc["tier"] | "official";
    const char* tag = doc["tag"] | "";
    if (std::strcmp(channel, "stable") == 0) out.channel = FwChannel::Stable;
    else if (std::strcmp(channel, "beta") == 0) out.channel = FwChannel::Beta;
    else return false;
    if (std::strcmp(tier, "official") == 0) out.tier = FwTier::Official;
    else if (std::strcmp(tier, "community") == 0) out.tier = FwTier::Community;
    else return false;
    if (!(tag[0] == '\0' || fwSafeName(tag, kFwTagMax))) return false;
    std::memcpy(out.name, name, std::strlen(name) + 1);
    std::memcpy(out.kit, kit, std::strlen(kit) + 1);
    std::memcpy(out.version, version, std::strlen(version) + 1);
    std::memcpy(out.tag, tag, std::strlen(tag) + 1);
    out.boardRev = static_cast<uint8_t>(rev);
    JsonObjectConst files = doc["files"].as<JsonObjectConst>();
    if (files.isNull()) return false;
    for (JsonPairConst f : files) {
        const char* fname = f.key().c_str();
        if (std::strcmp(fname, "CrossPad.bin") != 0 && std::strcmp(fname, "assets.bin") != 0) continue;
        if (out.fileCount >= 2) break;
        const uint32_t size = f.value()["size"] | 0u;
        const char* sha = f.value()["sha256"] | "";
        if (size == 0 || !isHex64(sha)) return false;
        FwManifestFile& dst = out.files[out.fileCount++];
        std::memcpy(dst.name, fname, std::strlen(fname) + 1);
        dst.size = size;
        std::memcpy(dst.sha256, sha, kShaHexLen + 1);
    }
    return fwManifestFile(out, "CrossPad.bin") != nullptr;
}

} // namespace

bool parseFwManifest(const char* json, size_t len, FwManifest& out, ArduinoJson::Allocator* alloc) {
    FwManifest parsed;
    if (!parseInto(json, len, parsed, alloc)) return false;
    out = parsed;
    return true;
}

const FwManifestFile* fwManifestFile(const FwManifest& m, const char* name) {
    for (size_t i = 0; i < m.fileCount; ++i)
        if (std::strcmp(m.files[i].name, name) == 0) return &m.files[i];
    return nullptr;
}

bool fwManifestMatches(const FwManifest& m, const char* kit, const char* catalogVersion, uint8_t boardRev) {
    Semver have, want;
    return std::strcmp(m.kit, kit) == 0 && m.boardRev == boardRev && Semver::parse(m.version, have) &&
           Semver::parse(catalogVersion, want) && have == want;
}

} // namespace crosspad

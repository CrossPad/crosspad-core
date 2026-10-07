#include "crosspad/fw/FwAccount.hpp"

#include <ArduinoJson.h>
#include <cstring>
#include "crosspad/fw/FwStore.hpp"   // fwCopyLine

namespace crosspad {
namespace {

constexpr int kHttpOk = 200;
constexpr int kHttpAccepted = 202;
constexpr int kHttpGone = 410;
constexpr const char kHttps[] = "https://";
constexpr const char kHttp[] = "http://";

// "http(s)://host[:port]" and nothing after it: a path, userinfo, query or
// fragment in the base would let the prefix compare match more than one host.
bool validBase(const char* b) {
    const char* host = nullptr;
    if (std::strncmp(b, kHttps, sizeof kHttps - 1) == 0) host = b + sizeof kHttps - 1;
    else if (std::strncmp(b, kHttp, sizeof kHttp - 1) == 0) host = b + sizeof kHttp - 1;
    if (!host || !*host) return false;
    for (const char* p = host; *p; ++p)
        if (*p == '/' || *p == '@' || *p == '?' || *p == '#') return false;
    return true;
}

bool lowerHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

} // namespace

bool fwAccountUrlAllowed(const char* accountBase, const char* url) {
    if (!accountBase || !url || !validBase(accountBase)) return false;
    const size_t n = std::strlen(accountBase);
    // The byte after the base must start the path: "…app.evil.com", "…app@evil",
    // "…:87650" all continue the host instead. "//" would be read as a new authority.
    return std::strncmp(url, accountBase, n) == 0 && url[n] == '/' && url[n + 1] != '/' && url[n + 1] != '\0';
}

bool fwPairCodeValid(const char* code) {
    if (!code || std::strlen(code) != kFwPairCodeLen) return false;
    for (size_t i = 0; i < kFwPairCodeLen; ++i)
        if (!std::strchr(kFwPairAlphabet, code[i])) return false;
    return true;
}

bool fwDeviceTokenValid(const char* token) {
    if (!token || std::strlen(token) != kFwDeviceTokenLen) return false;
    for (size_t i = 0; i < kFwDeviceTokenLen; ++i)
        if (!lowerHex(token[i])) return false;
    return true;
}

bool parseFwPairStart(const char* json, size_t len, FwPairStart& out) {
    if (!json || len == 0) return false;
    JsonDocument doc;
    if (deserializeJson(doc, json, len)) return false;
    const char* code = doc["code"] | "";
    const uint32_t expires = doc["expires_in"] | 0u;
    if (!fwPairCodeValid(code) || expires == 0) return false;
    std::memcpy(out.code, code, kFwPairCodeLen + 1);
    out.expiresIn = expires;
    return true;
}

FwPairReply parseFwPairToken(int status, const char* json, size_t len, FwPairToken& out) {
    if (status == kHttpAccepted) return FwPairReply::Waiting;
    if (status == kHttpGone) return FwPairReply::Expired;
    if (status != kHttpOk || !json || len == 0) return FwPairReply::Bad;
    JsonDocument doc;
    if (deserializeJson(doc, json, len)) return FwPairReply::Bad;
    const char* token = doc["token"] | "";
    if (!fwDeviceTokenValid(token)) return FwPairReply::Bad;
    std::memcpy(out.token, token, kFwDeviceTokenLen + 1);
    fwCopyLine(out.account, sizeof out.account, doc["account"] | "");
    return FwPairReply::Token;
}

} // namespace crosspad

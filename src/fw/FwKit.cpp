#include "crosspad/fw/FwKit.hpp"
#include <cstring>

namespace crosspad {

/// "v1" would read as a release tag in CrossPad-<tag>-<rev>-<kit>.bin.
static bool looksLikeVersionTag(const char* kit) {
    if (kit[0] != 'v' || !kit[1]) return false;
    for (const char* p = kit + 1; *p; ++p)
        if (*p < '0' || *p > '9') return false;
    return true;
}

bool fwKitIdValid(const char* kit) {
    if (!kit || kit[0] < 'a' || kit[0] > 'z' || looksLikeVersionTag(kit)) return false;
    size_t n = 0;
    for (const char* p = kit; *p; ++p) {
        if (++n >= kFwKitMax) return false;
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

bool fwBetaListHas(const char* list, const char* kit) {
    if (!list || !kit) return false;
    const size_t kl = std::strlen(kit);
    for (const char* p = list; *p;) {
        while (*p == ' ') ++p;
        const char* e = p;
        while (*e && *e != ' ') ++e;
        if (static_cast<size_t>(e - p) == kl && std::strncmp(p, kit, kl) == 0) return true;
        p = e;
    }
    return false;
}

bool fwBetaListSet(char* list, size_t cap, const char* kit, bool beta) {
    if (!fwKitIdValid(kit) || !list || cap == 0) return false;
    if (beta && fwBetaListHas(list, kit)) return true;   // keeps its place in the list
    char out[kFwBetaListMax] = "";
    size_t o = 0;
    const size_t kl = std::strlen(kit);
    for (const char* p = list; *p;) {
        while (*p == ' ') ++p;
        const char* e = p;
        while (*e && *e != ' ') ++e;
        const size_t n = static_cast<size_t>(e - p);
        if (n && !(n == kl && std::strncmp(p, kit, kl) == 0)) {
            if (o + (o ? 1 : 0) + n >= sizeof out) return false;
            if (o) out[o++] = ' ';
            std::memcpy(out + o, p, n);
            o += n;
            out[o] = '\0';
        }
        p = e;
    }
    if (beta) {
        if (o + (o ? 1 : 0) + kl >= sizeof out) return false;
        if (o) out[o++] = ' ';
        std::memcpy(out + o, kit, kl + 1);
        o += kl;
    }
    if (o + 1 > cap) return false;
    std::memcpy(list, out, o + 1);
    return true;
}

} // namespace crosspad

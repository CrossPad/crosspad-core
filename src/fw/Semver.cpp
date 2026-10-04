#include "crosspad/fw/Semver.hpp"
#include <cstdlib>
#include <cstring>

namespace crosspad {
namespace {

constexpr uint32_t kMaxPart = 65535;
constexpr size_t kRestMax = 64;
constexpr size_t kMinHashLen = 4;
constexpr size_t kMaxCommitDigits = 5;
constexpr char kDirty[] = "-dirty";

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isHexDigit(char c) { return isDigit(c) || (c >= 'a' && c <= 'f'); }

bool allDigits(const char* s, size_t n) {
    if (n == 0) return false;
    for (size_t i = 0; i < n; ++i) if (!isDigit(s[i])) return false;
    return true;
}

bool allHex(const char* s) {
    size_t n = 0;
    for (; s[n]; ++n) if (!isHexDigit(s[n])) return false;
    return n >= kMinHashLen;
}

bool readNumber(const char*& p, uint16_t& out) {
    if (!isDigit(*p)) return false;
    uint32_t v = 0;
    while (isDigit(*p)) {
        v = v * 10 + static_cast<uint32_t>(*p - '0');
        if (v > kMaxPart) return false;
        ++p;
    }
    out = static_cast<uint16_t>(v);
    return true;
}

/// Dot-separated identifiers of [0-9A-Za-z-], none empty: a pre-release or "+build".
bool validPre(const char* s) {
    if (!*s) return false;
    bool empty = true;
    for (const char* p = s; *p; ++p) {
        if (*p == '.') {
            if (empty) return false;
            empty = true;
        } else if (isDigit(*p) || isAlpha(*p) || *p == '-') {
            empty = false;
        } else {
            return false;
        }
    }
    return !empty;
}

/// "[<pre>-]<digits>-g<hex>" at the end of `rest`: strip it into `commits`.
void takeDescribeSuffix(char* rest, uint16_t& commits) {
    char* g = std::strrchr(rest, '-');
    if (!g || g[1] != 'g' || !allHex(g + 2)) return;
    *g = '\0';
    char* dash = std::strrchr(rest, '-');
    char* digits = dash ? dash + 1 : rest;
    const size_t n = std::strlen(digits);
    if (!allDigits(digits, n) || n > kMaxCommitDigits || std::strtoul(digits, nullptr, 10) > kMaxPart) {
        *g = '-';   // not a describe suffix after all
        return;
    }
    commits = static_cast<uint16_t>(std::strtoul(digits, nullptr, 10));
    if (dash) *dash = '\0';
    else rest[0] = '\0';
}

int sign(int v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); }

int compareNumeric(const char* a, size_t al, const char* b, size_t bl) {
    while (al > 1 && *a == '0') { ++a; --al; }
    while (bl > 1 && *b == '0') { ++b; --bl; }
    if (al != bl) return al < bl ? -1 : 1;
    return sign(std::strncmp(a, b, al));
}

size_t alphaPrefix(const char* s, size_t n) {
    size_t i = 0;
    while (i < n && isAlpha(s[i])) ++i;
    return i;
}

int compareIdent(const char* a, size_t al, const char* b, size_t bl) {
    const bool ad = allDigits(a, al), bd = allDigits(b, bl);
    if (ad && bd) return compareNumeric(a, al, b, bl);
    if (ad) return -1;
    if (bd) return 1;
    // "rc9" < "rc10": the same letters followed by digits only compare the digits as numbers.
    const size_t ap = alphaPrefix(a, al), bp = alphaPrefix(b, bl);
    if (ap > 0 && ap == bp && ap < al && bp < bl && std::strncmp(a, b, ap) == 0 &&
        allDigits(a + ap, al - ap) && allDigits(b + bp, bl - bp))
        return compareNumeric(a + ap, al - ap, b + bp, bl - bp);
    const size_t n = al < bl ? al : bl;
    const int c = std::strncmp(a, b, n);
    if (c) return sign(c);
    return al == bl ? 0 : (al < bl ? -1 : 1);
}

int comparePre(const char* a, const char* b) {
    for (;;) {
        const size_t al = std::strcspn(a, "."), bl = std::strcspn(b, ".");
        const int c = compareIdent(a, al, b, bl);
        if (c) return c;
        a += al;
        b += bl;
        if (!*a || !*b) return (*a ? 1 : 0) - (*b ? 1 : 0);
        ++a;
        ++b;
    }
}

} // namespace

bool Semver::parse(const char* text, Semver& out) {
    out = Semver{};
    if (!text) return false;
    Semver s;
    const char* p = text;
    if (*p == 'v' || *p == 'V') ++p;
    if (!readNumber(p, s.major) || *p != '.') return false;
    ++p;
    if (!readNumber(p, s.minor)) return false;
    if (*p == '.') {
        ++p;
        if (!readNumber(p, s.patch)) return false;
    }
    if (*p == '+' || *p == '\0') {   // "+build" metadata never orders versions
        if (*p == '+' && !validPre(p + 1)) return false;
        out = s;
        return true;
    }
    if (*p != '-') return false;
    ++p;
    char rest[kRestMax];
    const size_t n = std::strcspn(p, "+");
    if (n == 0 || n >= sizeof rest || (p[n] == '+' && !validPre(p + n + 1))) return false;
    std::memcpy(rest, p, n);
    rest[n] = '\0';
    const size_t dl = sizeof kDirty - 1;
    if (std::strcmp(rest, kDirty + 1) == 0) rest[0] = '\0';
    else if (n > dl && std::strcmp(rest + n - dl, kDirty) == 0) rest[n - dl] = '\0';
    takeDescribeSuffix(rest, s.commits);
    if (rest[0]) {
        if (!validPre(rest) || std::strlen(rest) >= kPreMax) return false;
        std::memcpy(s.pre, rest, std::strlen(rest) + 1);
    }
    out = s;
    return true;
}

int compareSemver(const Semver& a, const Semver& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    const bool ap = a.pre[0] != '\0', bp = b.pre[0] != '\0';
    if (ap != bp) return ap ? -1 : 1;
    if (ap) {
        const int c = comparePre(a.pre, b.pre);
        if (c) return c;
    }
    if (a.commits != b.commits) return a.commits < b.commits ? -1 : 1;
    return 0;
}

int compareVersionText(const char* a, const char* b) {
    Semver x, y;
    const bool okx = Semver::parse(a, x), oky = Semver::parse(b, y);
    if (!okx || !oky) return okx == oky ? 0 : (okx ? 1 : -1);
    return compareSemver(x, y);
}

} // namespace crosspad

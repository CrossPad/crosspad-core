// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/net/RemoteFrame.hpp"
#include <cstring>

namespace crosspad {
namespace {
constexpr char kTruncated[] = "ERR truncated\r\n";
bool idChar(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'); }
}  // namespace

bool parseRemoteRequest(const char* payload, size_t len, RemoteRequest& out) {
    if (!payload) return false;
    size_t i = 0;
    while (i < len && payload[i] != ' ') {
        if (i >= kRemoteIdMax || !idChar(payload[i])) return false;
        out.id[i] = payload[i];
        ++i;
    }
    if (i == 0 || i >= len) return false;
    out.id[i] = '\0';
    const size_t start = i + 1, n = len - start;
    if (n == 0 || n > kRemoteCommandMax) return false;
    for (size_t k = 0; k < n; ++k) {
        const char c = payload[start + k];
        if (c < 0x20 || c > 0x7e) return false;
        out.command[k] = c;
    }
    out.command[n] = '\0';
    return true;
}

size_t formatRemoteReply(const char* id, const char* reply, char* out, size_t cap) {
    if (cap == 0) return 0;
    size_t n = 0;
    auto put = [&](const char* s, size_t len) {
        const size_t room = cap - 1 - n;
        const size_t k = len < room ? len : room;
        std::memcpy(out + n, s, k);
        n += k;
    };
    put(id, std::strlen(id));
    put(" ", 1);
    const size_t head = n;
    const size_t len = std::strlen(reply);
    const size_t room = cap - 1 - head;
    const size_t limit = room < kRemoteReplyMax ? room : kRemoteReplyMax;
    if (len <= limit) {
        put(reply, len);
    } else {
        const size_t mark = sizeof kTruncated - 1;
        size_t keep = limit > mark ? limit - mark : 0;
        while (keep > 0 && reply[keep - 1] != '\n') --keep;   // whole lines only
        put(reply, keep);
        put(kTruncated, mark);
    }
    out[n] = '\0';
    return n;
}

}  // namespace crosspad

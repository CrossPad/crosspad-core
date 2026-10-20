// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>

namespace crosspad {

/// Next whitespace-separated token of a command line; a token in double
/// quotes may contain spaces ("" is an empty token). No escapes: a quote
/// character cannot be part of a quoted token. Advances `p`. False at the end
/// of the line, on an unterminated quote, or when the token does not fit `cap`.
inline bool nextQuotedArg(const char*& p, char* out, size_t cap) {
    while (*p == ' ' || *p == '\t') ++p;
    if (!*p || cap == 0) return false;
    size_t n = 0;
    if (*p == '"') {
        ++p;
        while (*p && *p != '"') {
            if (n + 1 >= cap) return false;
            out[n++] = *p++;
        }
        if (*p != '"') return false;
        ++p;
    } else {
        while (*p && *p != ' ' && *p != '\t') {
            if (n + 1 >= cap) return false;
            out[n++] = *p++;
        }
    }
    out[n] = '\0';
    return true;
}

} // namespace crosspad

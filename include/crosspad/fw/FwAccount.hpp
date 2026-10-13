// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace crosspad {

/// A device token: 32 random bytes as lower-case hex (crosspad.app /api/device/pair/token).
constexpr size_t kFwDeviceTokenLen = 64;
/// A pairing code: 6 characters of kFwPairAlphabet, valid 10 minutes, single use.
constexpr size_t kFwPairCodeLen = 6;
/// No 0/O or 1/I: a code is read off the LCD and typed on the website.
constexpr const char* kFwPairAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
/// The account's display name as the board stores it (NVS cpacct/name), bytes incl. NUL.
constexpr size_t kFwAccountNameMax = 32;
/// The board's identity towards the account: the factory MAC, 12 lower-case hex.
constexpr size_t kFwBoardSerialLen = 12;
/// How often the board asks whether its code was claimed.
constexpr uint32_t kFwPairPollMs = 3000;

/// True only for `url` = `accountBase` + "/" + something: same scheme, host and
/// port byte for byte, so a look-alike host, userinfo or another port never
/// matches. `accountBase` is "http(s)://host[:port]" with no path.
bool fwAccountUrlAllowed(const char* accountBase, const char* url);
/// kFwPairCodeLen characters, every one in kFwPairAlphabet.
bool fwPairCodeValid(const char* code);
/// kFwDeviceTokenLen lower-case hex digits.
bool fwDeviceTokenValid(const char* token);

/// The poll secret pair/start hands the board (64 lower-case hex): pair/token mints
/// a token only with it, so the code shown on the screen alone gets nobody a token.
constexpr size_t kFwPairPollLen = 64;

struct FwPairStart {
    char code[kFwPairCodeLen + 1];
    char poll[kFwPairPollLen + 1];   ///< secret: never shown, never logged
    uint32_t expiresIn;   ///< seconds
};
/// The 200 body of pair/start; false for a malformed code or poll secret, or no
/// expiry, `out` then untouched.
bool parseFwPairStart(const char* json, size_t len, FwPairStart& out);

enum class FwPairReply : uint8_t { Waiting, Token, Expired, Bad };
struct FwPairToken {
    char token[kFwDeviceTokenLen + 1];
    char account[kFwAccountNameMax];   ///< display text, one line
};
/// 202 Waiting, 410 Expired, 200 with a valid token Token, anything else Bad.
FwPairReply parseFwPairToken(int status, const char* json, size_t len, FwPairToken& out);

} // namespace crosspad

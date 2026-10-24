// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace crosspad {

/// One SHA-256 digest at a time. The platform backs it with whatever it has
/// (mbedtls and the SHA accelerator on ESP-IDF); core only feeds bytes.
class ISha256 {
public:
    static constexpr size_t kDigestLen = 32;

    virtual ~ISha256() = default;
    /// Begins a new digest, dropping one in progress.
    virtual void start() = 0;
    virtual void update(const uint8_t* data, size_t n) = 0;
    /// Ends the digest begun by start().
    virtual void finish(uint8_t digest[kDigestLen]) = 0;
};

} // namespace crosspad

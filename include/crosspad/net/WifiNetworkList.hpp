#pragma once
#include <cstddef>
#include <cstdint>

namespace crosspad {

struct WifiNetwork {
    char ssid[33];
    char pass[65];
};

/// The networks a board remembers, most recently connected first. Pure data:
/// the platform persists the blob (serialize/deserialize) wherever it keeps
/// device-wide state.
class WifiNetworkList {
public:
    static constexpr size_t kMax = 5;
    static constexpr size_t kMaxSsid = 32;   // 802.11 SSID limit
    static constexpr size_t kMaxPass = 64;   // WPA2 passphrase / PSK hex limit
    static constexpr uint8_t kBlobVersion = 1;
    /// version + count + per network (len + ssid + len + pass)
    static constexpr size_t kBlobMax = 2 + kMax * (2 + kMaxSsid + kMaxPass);

    size_t size() const { return count_; }
    const WifiNetwork& at(size_t i) const { return nets_[i]; }
    /// Index of `ssid`, or -1.
    int find(const char* ssid) const;
    /// Insert or update, then move to the front; drops the oldest past kMax.
    /// False for an empty SSID or a field past its limit.
    bool put(const char* ssid, const char* pass);
    /// Move an existing network to the front.
    bool promote(const char* ssid);
    bool forget(const char* ssid);
    void clear() { count_ = 0; }

    /// Bytes written, 0 when `cap` is too small.
    size_t serialize(uint8_t* out, size_t cap) const;
    /// Replaces the list. On any malformed input the list is left empty and
    /// false returned — a foreign or truncated blob is never half-loaded.
    bool deserialize(const uint8_t* in, size_t len);

private:
    void moveToFront(size_t i);
    WifiNetwork nets_[kMax] = {};
    size_t count_ = 0;
};

} // namespace crosspad

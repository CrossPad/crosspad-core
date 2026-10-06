#include "crosspad/net/WifiNetworkList.hpp"
#include <cstring>
#include <initializer_list>

namespace crosspad {

int WifiNetworkList::find(const char* ssid) const {
    for (size_t i = 0; i < count_; ++i)
        if (std::strcmp(nets_[i].ssid, ssid) == 0) return (int)i;
    return -1;
}

void WifiNetworkList::moveToFront(size_t i) {
    if (i == 0) return;
    WifiNetwork n = nets_[i];
    std::memmove(&nets_[1], &nets_[0], i * sizeof(WifiNetwork));
    nets_[0] = n;
}

bool WifiNetworkList::put(const char* ssid, const char* pass) {
    if (!ssid || !pass) return false;
    size_t sl = std::strlen(ssid), pl = std::strlen(pass);
    if (sl == 0 || sl > kMaxSsid || pl > kMaxPass) return false;
    int idx = find(ssid);
    size_t slot;
    if (idx >= 0) {
        slot = (size_t)idx;
    } else if (count_ < kMax) {
        slot = count_++;
    } else {
        slot = kMax - 1;                     // overwrite the oldest
    }
    std::memcpy(nets_[slot].ssid, ssid, sl + 1);
    std::memcpy(nets_[slot].pass, pass, pl + 1);
    moveToFront(slot);
    return true;
}

bool WifiNetworkList::promote(const char* ssid) {
    int idx = find(ssid);
    if (idx < 0) return false;
    moveToFront((size_t)idx);
    return true;
}

bool WifiNetworkList::forget(const char* ssid) {
    int idx = find(ssid);
    if (idx < 0) return false;
    std::memmove(&nets_[idx], &nets_[idx + 1], (count_ - idx - 1) * sizeof(WifiNetwork));
    --count_;
    return true;
}

size_t WifiNetworkList::serialize(uint8_t* out, size_t cap) const {
    size_t need = 2;
    for (size_t i = 0; i < count_; ++i)
        need += 2 + std::strlen(nets_[i].ssid) + std::strlen(nets_[i].pass);
    if (!out || cap < need) return 0;
    size_t o = 0;
    out[o++] = kBlobVersion;
    out[o++] = (uint8_t)count_;
    for (size_t i = 0; i < count_; ++i) {
        for (const char* f : {nets_[i].ssid, nets_[i].pass}) {
            size_t l = std::strlen(f);
            out[o++] = (uint8_t)l;
            std::memcpy(out + o, f, l);
            o += l;
        }
    }
    return o;
}

bool WifiNetworkList::deserialize(const uint8_t* in, size_t len) {
    count_ = 0;
    if (!in || len < 2 || in[0] != kBlobVersion || in[1] > kMax) return false;
    WifiNetwork tmp[kMax] = {};
    size_t n = in[1], o = 2;
    for (size_t i = 0; i < n; ++i) {
        char* fields[2] = {tmp[i].ssid, tmp[i].pass};
        const size_t limits[2] = {kMaxSsid, kMaxPass};
        for (int f = 0; f < 2; ++f) {
            if (o >= len) return false;
            size_t l = in[o++];
            if (l > limits[f] || (f == 0 && l == 0) || o + l > len) return false;
            std::memcpy(fields[f], in + o, l);
            fields[f][l] = '\0';
            o += l;
        }
    }
    if (o != len) return false;
    std::memcpy(nets_, tmp, sizeof(tmp));
    count_ = n;
    return true;
}

} // namespace crosspad

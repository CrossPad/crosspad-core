#pragma once
#include <cstdint>

namespace crosspad {

/// Where the platform's leased WiFi link is. Mirrored into
/// CrosspadStatus::wifiState for the status bar.
enum class WifiLinkState : uint8_t { Off, Starting, Connecting, Up, Stopping, Failed };

/// Why the link last failed; None while it has not.
enum class WifiFail : uint8_t { None, Disabled, NoMem, NoNetwork, Auth, Timeout, Lost, TransferMode };

inline const char* wifiLinkStateName(WifiLinkState s) {
    switch (s) {
        case WifiLinkState::Off:        return "off";
        case WifiLinkState::Starting:   return "starting";
        case WifiLinkState::Connecting: return "connecting";
        case WifiLinkState::Up:         return "up";
        case WifiLinkState::Stopping:   return "stopping";
        case WifiLinkState::Failed:     return "failed";
    }
    return "?";
}

inline const char* wifiFailName(WifiFail f) {
    switch (f) {
        case WifiFail::None:         return "none";
        case WifiFail::Disabled:     return "disabled";
        case WifiFail::NoMem:        return "no_mem";
        case WifiFail::NoNetwork:    return "no_network";
        case WifiFail::Auth:         return "auth";
        case WifiFail::Timeout:      return "timeout";
        case WifiFail::Lost:         return "lost";
        case WifiFail::TransferMode: return "transfer_mode";
    }
    return "?";
}

/// Status-bar bars for a received signal strength.
constexpr int8_t kWifiBars4Dbm = -55;
constexpr int8_t kWifiBars3Dbm = -65;
constexpr int8_t kWifiBars2Dbm = -75;

inline uint8_t wifiRssiBars(int8_t rssi) {
    if (rssi >= kWifiBars4Dbm) return 4;
    if (rssi >= kWifiBars3Dbm) return 3;
    if (rssi >= kWifiBars2Dbm) return 2;
    return 1;
}

} // namespace crosspad

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace crosspad {

/// Where the platform's leased WiFi link is. Mirrored into
/// CrosspadStatus::wifiState.
enum class WifiLinkState : uint8_t { Off, Starting, Connecting, Up, Stopping, Failed };

/// Why the link last failed; None while it has not.
enum class WifiFail : uint8_t { None, Disabled, NoMem, NoNetwork, Auth, Timeout, Lost, TransferMode,
                                NotNetworkMode };

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
        case WifiFail::TransferMode:   return "transfer_mode";
        case WifiFail::NotNetworkMode: return "not_network_mode";
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

/// How the four WiFi bars look (spec §3): `lit` bars solid, the rest dimmed;
/// `firstRed` paints bar 1 red over the dimmed set after a failure.
struct WifiBarsLook {
    bool visible;
    uint8_t lit;
    bool firstRed;
};

/// One more bar every step while connecting, looping 1..kWifiBarCount.
constexpr uint32_t kWifiConnectStepMs = 250;
constexpr uint8_t kWifiBarCount = 4;

inline WifiBarsLook wifiBarsLook(bool allowed, WifiLinkState state, int8_t rssi, uint32_t tickMs) {
    if (!allowed) return {false, 0, false};
    switch (state) {
        case WifiLinkState::Up:
            return {true, wifiRssiBars(rssi), false};
        case WifiLinkState::Starting:
        case WifiLinkState::Connecting:
            return {true, static_cast<uint8_t>(1 + (tickMs / kWifiConnectStepMs) % kWifiBarCount), false};
        case WifiLinkState::Failed:
            return {true, 0, true};
        default:
            return {true, 0, false};
    }
}

} // namespace crosspad

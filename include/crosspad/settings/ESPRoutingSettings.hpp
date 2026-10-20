// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <ArduinoJson.h>

namespace crosspad {

class ESPRoutingSettings {
public:
    bool BLE2Midi;
    bool BLE2Audio;
    bool Midi2BLE;
    bool Midi2Audio;

    ESPRoutingSettings() {
        BLE2Midi = true;
        BLE2Audio = true;
        Midi2BLE = true;
        Midi2Audio = true;
    }

    void toJson(JsonObject& json) const {
        json["BLE2Midi"] = BLE2Midi;
        json["BLE2Audio"] = BLE2Audio;
        json["Midi2BLE"] = Midi2BLE;
        json["Midi2Audio"] = Midi2Audio;
    }

    void fromJson(const JsonObject& json) {
        BLE2Midi = json["BLE2Midi"];
        BLE2Audio = json["BLE2Audio"];
        Midi2BLE = json["Midi2BLE"];
        Midi2Audio = json["Midi2Audio"];
    }
};

} // namespace crosspad

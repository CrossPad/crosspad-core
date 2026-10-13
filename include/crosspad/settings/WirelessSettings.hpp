// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <ArduinoJson.h>

namespace crosspad {

class WirelessSettings {
public:
    bool enableWiFi;
    bool enableBLE;
    bool enableRemote;  ///< WiFi remote management (MQTT through crosspad.app); holds WiFi, so BLE is off while it runs
    bool enableOSC;
    bool enableOSCServer;
    bool enableUDP;
    bool enableTCP;
    bool enableWebServer;
    /// 0=Host (Central), 1=Server (Peripheral). A pad controller is something
    /// a phone or a DAW connects *to*, so it advertises by default; Host mode
    /// is for driving another BLE MIDI peripheral from the pad.
    uint8_t bleMidiMode = 1;
    int8_t  bleMidiNoteOffset = 0; ///< Semitone offset for BLE MIDI input
    /// Index into crosspad::kBleMidiTxOffsets — the transpose applied to notes
    /// *sent* over BLE. 0 = Default, i.e. the same note USB MIDI gets.
    uint8_t bleMidiTxOffset = 0;

    WirelessSettings() {
        enableWiFi = false;
        enableBLE = false;
        enableRemote = true;
        enableOSC = false;
        enableOSCServer = true;
        enableUDP = true;
        enableTCP = true;
        enableWebServer = true;
    }

    void toJson(JsonObject& json) const {
        json["enableWiFi"] = enableWiFi;
        json["enableBLE"] = enableBLE;
        json["enableRemote"] = enableRemote;
        json["enableOSC"] = enableOSC;
        json["enableOSCServer"] = enableOSCServer;
        json["enableUDP"] = enableUDP;
        json["enableTCP"] = enableTCP;
        json["enableWebServer"] = enableWebServer;
        json["bleMidiMode"] = bleMidiMode;
        json["bleMidiNoteOffset"] = bleMidiNoteOffset;
        json["bleMidiTxOffset"] = bleMidiTxOffset;
    }

    void fromJson(const JsonObject& json) {
        enableWiFi = json["enableWiFi"];
        enableBLE = json["enableBLE"];
        if (json["enableRemote"].is<bool>())
            enableRemote = json["enableRemote"].as<bool>();
        enableOSC = json["enableOSC"];
        enableOSCServer = json["enableOSCServer"];
        enableUDP = json["enableUDP"];
        enableTCP = json["enableTCP"];
        enableWebServer = json["enableWebServer"];
        if (json["bleMidiMode"].is<uint8_t>())
            bleMidiMode = json["bleMidiMode"].as<uint8_t>();
        if (json["bleMidiNoteOffset"].is<int8_t>())
            bleMidiNoteOffset = json["bleMidiNoteOffset"].as<int8_t>();
        if (json["bleMidiTxOffset"].is<uint8_t>())
            bleMidiTxOffset = json["bleMidiTxOffset"].as<uint8_t>();
    }
};

} // namespace crosspad

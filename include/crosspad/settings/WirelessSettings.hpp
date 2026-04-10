#pragma once
#include <cstdint>
#include <ArduinoJson.h>

namespace crosspad {

class WirelessSettings {
public:
    bool enableWiFi;
    bool enableBLE;
    bool enableOSC;
    bool enableOSCServer;
    bool enableUDP;
    bool enableTCP;
    bool enableWebServer;
    uint8_t bleMidiMode = 0;       ///< 0=Host (Central), 1=Server (Peripheral)
    int8_t  bleMidiNoteOffset = 0; ///< Semitone offset for BLE MIDI input

    WirelessSettings() {
        enableWiFi = false;
        enableBLE = true;
        enableOSC = false;
        enableOSCServer = true;
        enableUDP = true;
        enableTCP = true;
        enableWebServer = true;
    }

    void toJson(JsonObject& json) const {
        json["enableWiFi"] = enableWiFi;
        json["enableBLE"] = enableBLE;
        json["enableOSC"] = enableOSC;
        json["enableOSCServer"] = enableOSCServer;
        json["enableUDP"] = enableUDP;
        json["enableTCP"] = enableTCP;
        json["enableWebServer"] = enableWebServer;
        json["bleMidiMode"] = bleMidiMode;
        json["bleMidiNoteOffset"] = bleMidiNoteOffset;
    }

    void fromJson(const JsonObject& json) {
        enableWiFi = json["enableWiFi"];
        enableBLE = json["enableBLE"];
        enableOSC = json["enableOSC"];
        enableOSCServer = json["enableOSCServer"];
        enableUDP = json["enableUDP"];
        enableTCP = json["enableTCP"];
        enableWebServer = json["enableWebServer"];
        if (json.containsKey("bleMidiMode"))
            bleMidiMode = json["bleMidiMode"].as<uint8_t>();
        if (json.containsKey("bleMidiNoteOffset"))
            bleMidiNoteOffset = json["bleMidiNoteOffset"].as<int8_t>();
    }
};

} // namespace crosspad

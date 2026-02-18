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
    }

    void fromJson(const JsonObject& json) {
        enableWiFi = json["enableWiFi"];
        enableBLE = json["enableBLE"];
        enableOSC = json["enableOSC"];
        enableOSCServer = json["enableOSCServer"];
        enableUDP = json["enableUDP"];
        enableTCP = json["enableTCP"];
        enableWebServer = json["enableWebServer"];
    }
};

} // namespace crosspad

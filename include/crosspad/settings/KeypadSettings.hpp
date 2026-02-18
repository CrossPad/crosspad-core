#pragma once
#include <cstdint>
#include <ArduinoJson.h>

namespace crosspad {

class KeypadSettings {
public:
    bool enableKeypad;
    bool noteOffOnRelease;
    bool inactiveLights;
    bool lightsOnNoteOn;
    bool lightsOnNoteOff;
    bool upperRowFunctions;
    bool ecoMode;
    bool send2STM;
    bool send2BLE;
    bool send2USB;
    bool sendCC;

    KeypadSettings() {
        enableKeypad = true;
        noteOffOnRelease = false;
        inactiveLights = true;
        lightsOnNoteOn = false;
        lightsOnNoteOff = false;
        upperRowFunctions = false;
        ecoMode = false;
        send2STM = false;
        send2BLE = true;
        send2USB = true;
        sendCC = false;
    }

    void toJson(JsonObject& json) const {
        json["enableKeypad"] = enableKeypad;
        json["inactiveLights"] = inactiveLights;
        json["ecoMode"] = ecoMode;
        json["send2STM"] = send2STM;
        json["send2BLE"] = send2BLE;
        json["send2USB"] = send2USB;
        json["sendCC"] = sendCC;
    }

    void fromJson(const JsonObject& json) {
        enableKeypad = json["enableKeypad"];
        inactiveLights = json["inactiveLights"];
        ecoMode = json["ecoMode"];
        send2STM = json["send2STM"];
        send2BLE = json["send2BLE"];
        send2USB = json["send2USB"];
        sendCC = json["sendCC"];
    }
};

} // namespace crosspad

#pragma once
#include <cstdint>
#include <ArduinoJson.h>

namespace crosspad {

class VibrationSettings {
public:
    bool enable;
    bool enableVibrationOnTouch;
    bool enableVibrationOnError;
    bool enableAudioToVibe;
    uint8_t inputMin;
    uint8_t inputMax;
    uint8_t outputMin;
    uint8_t outputMax;
    bool debugging;

    VibrationSettings() {
        enable = true;
        enableVibrationOnTouch = true;
        enableVibrationOnError = true;
        enableAudioToVibe = true;
        inputMin = 10;
        inputMax = 127;
        outputMin = 5;
        outputMax = 200;
        debugging = false;
    }

    void toJson(JsonObject& json) const {
        json["enable"] = enable;
        json["enableVibrationOnTouch"] = enableVibrationOnTouch;
        json["enableVibrationOnError"] = enableVibrationOnError;
        json["enableAudioToVibe"] = enableAudioToVibe;
        json["inputMin"] = inputMin;
        json["inputMax"] = inputMax;
        json["outputMin"] = outputMin;
        json["outputMax"] = outputMax;
        json["debugging"] = debugging;
    }

    void fromJson(const JsonObject& json) {
        enable = json["enable"];
        enableVibrationOnTouch = json["enableVibrationOnTouch"];
        enableVibrationOnError = json["enableVibrationOnError"];
        enableAudioToVibe = json["enableAudioToVibe"];
        inputMin = json["inputMin"];
        inputMax = json["inputMax"];
        outputMin = json["outputMin"];
        outputMax = json["outputMax"];
        debugging = json["debugging"];
    }
};

} // namespace crosspad

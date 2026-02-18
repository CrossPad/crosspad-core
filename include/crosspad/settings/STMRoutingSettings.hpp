#pragma once
#include <cstdint>
#include <ArduinoJson.h>

namespace crosspad {

class STMRoutingSettings {
public:
    bool Midi2MidiThru;
    bool Midi2USB;
    bool Midi2ESP;
    bool USB2Midi;
    bool USB2USBThru;
    bool USB2ESP;
    bool ESP2Midi;
    bool ESP2USB;
    bool ESP2ESPThru;
    bool RoutingMatrixEnabled;
    bool ConfigButtonsAsMidi;
    bool Buttons2Midi;
    bool Buttons2USB;
    bool Buttons2ESP;

    STMRoutingSettings() {
        RoutingMatrixEnabled = true;
        ConfigButtonsAsMidi = false;
        Midi2ESP = false;
        Midi2USB = true;
        Midi2MidiThru = false;
        USB2ESP = true;
        USB2Midi = false;
        USB2USBThru = false;
        ESP2Midi = true;
        ESP2USB = true;
        ESP2ESPThru = false;
        Buttons2Midi = false;
        Buttons2USB = false;
        Buttons2ESP = true;
    }

    void toJson(JsonObject& json) const {
        json["Midi2MidiThru"] = Midi2MidiThru;
        json["Midi2USB"] = Midi2USB;
        json["Midi2ESP"] = Midi2ESP;
        json["USB2Midi"] = USB2Midi;
        json["USB2USBThru"] = USB2USBThru;
        json["USB2ESP"] = USB2ESP;
        json["ESP2Midi"] = ESP2Midi;
        json["ESP2USB"] = ESP2USB;
        json["ESP2ESPThru"] = ESP2ESPThru;
        json["RoutingMatrixEnabled"] = RoutingMatrixEnabled;
        json["ConfigButtonsAsMidi"] = ConfigButtonsAsMidi;
        json["Buttons2Midi"] = Buttons2Midi;
        json["Buttons2USB"] = Buttons2USB;
        json["Buttons2ESP"] = Buttons2ESP;
    }

    void fromJson(const JsonObject& json) {
        Midi2MidiThru = json["Midi2MidiThru"];
        Midi2USB = json["Midi2USB"];
        Midi2ESP = json["Midi2ESP"];
        USB2Midi = json["USB2Midi"];
        USB2USBThru = json["USB2USBThru"];
        USB2ESP = json["USB2ESP"];
        ESP2Midi = json["ESP2Midi"];
        ESP2USB = json["ESP2USB"];
        ESP2ESPThru = json["ESP2ESPThru"];
        RoutingMatrixEnabled = json["RoutingMatrixEnabled"];
        ConfigButtonsAsMidi = json["ConfigButtonsAsMidi"];
        Buttons2Midi = json["Buttons2Midi"];
        Buttons2USB = json["Buttons2USB"];
        Buttons2ESP = json["Buttons2ESP"];
    }

    void SetUsbPassThru(bool state) {
        USB2ESP = state;
        ESP2USB = state;
        if (state) {
            USB2USBThru = false;
        }
    }
};

} // namespace crosspad

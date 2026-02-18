#pragma once
#include <cstdint>
#include <ArduinoJson.h>

namespace crosspad {

class STMRoutingSettings {
public:
    // ── Byte 1 bit positions (7 bools, bits 0-6) ──
    static constexpr uint8_t BIT_MIDI2MIDI_THRU  = 0x01;
    static constexpr uint8_t BIT_MIDI2USB        = 0x02;
    static constexpr uint8_t BIT_MIDI2ESP        = 0x04;
    static constexpr uint8_t BIT_USB2MIDI        = 0x08;
    static constexpr uint8_t BIT_USB2USB_THRU    = 0x10;
    static constexpr uint8_t BIT_USB2ESP         = 0x20;
    static constexpr uint8_t BIT_ESP2MIDI        = 0x40;

    // ── Byte 2 bit positions (7 bools, bits 0-6) ──
    static constexpr uint8_t BIT_ESP2USB         = 0x01;
    static constexpr uint8_t BIT_ESP2ESP_THRU    = 0x02;
    static constexpr uint8_t BIT_ENABLED         = 0x04;
    static constexpr uint8_t BIT_BUTTONS_AS_MIDI = 0x08;
    static constexpr uint8_t BIT_BUTTONS2MIDI    = 0x10;
    static constexpr uint8_t BIT_BUTTONS2USB     = 0x20;
    static constexpr uint8_t BIT_BUTTONS2ESP     = 0x40;

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

    /** Pack first 7 routing bools into a single byte for SysEx transmission. */
    uint8_t packByte1() const {
        uint8_t b = 0;
        if (Midi2MidiThru)  b |= BIT_MIDI2MIDI_THRU;
        if (Midi2USB)       b |= BIT_MIDI2USB;
        if (Midi2ESP)       b |= BIT_MIDI2ESP;
        if (USB2Midi)       b |= BIT_USB2MIDI;
        if (USB2USBThru)    b |= BIT_USB2USB_THRU;
        if (USB2ESP)        b |= BIT_USB2ESP;
        if (ESP2Midi)       b |= BIT_ESP2MIDI;
        return b;
    }

    /** Pack remaining 7 routing bools into a single byte for SysEx transmission. */
    uint8_t packByte2() const {
        uint8_t b = 0;
        if (ESP2USB)              b |= BIT_ESP2USB;
        if (ESP2ESPThru)          b |= BIT_ESP2ESP_THRU;
        if (RoutingMatrixEnabled) b |= BIT_ENABLED;
        if (ConfigButtonsAsMidi)  b |= BIT_BUTTONS_AS_MIDI;
        if (Buttons2Midi)         b |= BIT_BUTTONS2MIDI;
        if (Buttons2USB)          b |= BIT_BUTTONS2USB;
        if (Buttons2ESP)          b |= BIT_BUTTONS2ESP;
        return b;
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

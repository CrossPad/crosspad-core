// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <ArduinoJson.h>
#include "crosspad/imu/ImuMidiMapper.hpp"

namespace crosspad {

/**
 * @brief Persisted shape of the IMU-to-MIDI mapping.
 *
 * Off by default. Tilt silently emitting controllers on a board nobody asked to
 * behave that way would look like a fault, so this is something you turn on.
 *
 * The defaults describe the mapping worth having first rather than an empty
 * table: pitch and roll on CC 1 and CC 74, the two controllers a host is most
 * likely to already have bound to something. The gyro slots exist but start
 * disabled — angular rate is a different instrument and deserves a deliberate
 * choice.
 */
class ImuMidiSettings {
public:
    bool    enabled;
    uint8_t minIntervalMs;   ///< per-slot floor between messages; 20 = 50 Hz

    struct Slot {
        bool    enabled;
        uint8_t source;      ///< ImuMidiSource
        uint8_t target;      ///< ImuMidiTarget
        uint8_t cc;
        uint8_t channel;
        bool    invert;
        float   inputSpan;   ///< degrees for angles, deg/s for the gyro
    } slots[kImuMidiSlots];

    ImuMidiSettings() {
        enabled = false;
        minIntervalMs = (uint8_t)ImuMidiMapper::kDefaultMinIntervalMs;

        for (auto &s : slots) {
            s = Slot{false, (uint8_t)ImuMidiSource::Pitch,
                     (uint8_t)ImuMidiTarget::ControlChange, 1, 0, false, 45.0f};
        }
        // Tilt forward/back -> CC 1 (mod wheel), left/right -> CC 74 (filter).
        slots[0] = Slot{true, (uint8_t)ImuMidiSource::Pitch,
                        (uint8_t)ImuMidiTarget::ControlChange, 1, 0, false, 45.0f};
        slots[1] = Slot{true, (uint8_t)ImuMidiSource::Roll,
                        (uint8_t)ImuMidiTarget::ControlChange, 74, 0, false, 45.0f};
        // Gyro: present, off, and wider-ranged because rate swings far further
        // than attitude does.
        slots[2] = Slot{false, (uint8_t)ImuMidiSource::GyroZ,
                        (uint8_t)ImuMidiTarget::ControlChange, 11, 0, false, 250.0f};
        slots[3] = Slot{false, (uint8_t)ImuMidiSource::GyroX,
                        (uint8_t)ImuMidiTarget::PitchBend, 0, 0, false, 250.0f};
    }

    void toJson(JsonObject& json) const {
        json["enabled"] = enabled;
        json["minIntervalMs"] = minIntervalMs;
        JsonArray arr = json["slots"].to<JsonArray>();
        for (const auto &s : slots) {
            JsonObject o = arr.add<JsonObject>();
            o["enabled"] = s.enabled;
            o["source"] = s.source;
            o["target"] = s.target;
            o["cc"] = s.cc;
            o["channel"] = s.channel;
            o["invert"] = s.invert;
            o["inputSpan"] = s.inputSpan;
        }
    }

    void fromJson(const JsonObject& json) {
        if (!json["enabled"].isNull()) enabled = json["enabled"];
        if (!json["minIntervalMs"].isNull()) minIntervalMs = json["minIntervalMs"];
        JsonArrayConst arr = json["slots"];
        size_t i = 0;
        for (JsonObjectConst o : arr) {
            if (i >= kImuMidiSlots) break;
            Slot &s = slots[i++];
            if (!o["enabled"].isNull())   s.enabled  = o["enabled"];
            if (!o["source"].isNull())    s.source   = o["source"];
            if (!o["target"].isNull())    s.target   = o["target"];
            if (!o["cc"].isNull())        s.cc       = o["cc"];
            if (!o["channel"].isNull())   s.channel  = o["channel"];
            if (!o["invert"].isNull())    s.invert   = o["invert"];
            if (!o["inputSpan"].isNull()) s.inputSpan = o["inputSpan"];
        }
    }

    /// Copy this table into a mapper. One place, so the verbs, the UI and the
    /// boot path cannot each apply it slightly differently.
    void applyTo(ImuMidiMapper& m) const {
        m.setMinIntervalMs(minIntervalMs);
        for (size_t i = 0; i < kImuMidiSlots; ++i) {
            ImuMidiSlot &d = m.slot(i);
            d.enabled   = slots[i].enabled;
            d.source    = (ImuMidiSource)(slots[i].source < (uint8_t)ImuMidiSource::Count
                                          ? slots[i].source : 0);
            d.target    = (ImuMidiTarget)(slots[i].target < (uint8_t)ImuMidiTarget::Count
                                          ? slots[i].target : 0);
            d.cc        = slots[i].cc & 0x7F;
            d.channel   = slots[i].channel & 0x0F;
            d.invert    = slots[i].invert;
            d.inputSpan = slots[i].inputSpan;
        }
        m.setEnabled(enabled);   // last: it clears the per-slot memory
    }
};

} // namespace crosspad

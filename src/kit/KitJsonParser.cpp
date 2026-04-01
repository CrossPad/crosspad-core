// SPDX-License-Identifier: MIT
#include <crosspad/kit/KitJsonParser.hpp>

namespace crosspad {

bool kit_parse_metadata(KitInfo& kit, JsonDocument& doc)
{
    kit.name        = doc["name"]        | "Kit";
    kit.author      = doc["author"]      | "";
    kit.description = doc["description"] | "";
    kit.version     = doc["version"]     | "";
    kit.created_at  = doc["created_at"]  | "";

    if (doc["type"].is<int>())
        kit.type = static_cast<KitInfo::Type>(doc["type"].as<int>());

    return true;
}

bool kit_parse_pads(KitInfo& kit, JsonDocument& doc)
{
    JsonArray padsArr = doc["pads"].as<JsonArray>();
    if (padsArr.isNull()) return false;

    kit.total_samples = 0;

    int padIdx = 0;
    for (JsonObject padObj : padsArr) {
        if (padIdx >= KIT_PADS) break;

        Pad& pad    = kit.pads[padIdx];
        pad.number  = padIdx;
        pad.note    = padObj["note"]          | (uint8_t)padIdx;
        pad.color   = padObj["color"]         | 0x20FF10u;
        pad.color_pressed = padObj["color_pressed"] | 0xFF1020u;
        pad.choke_group   = padObj["choke_group"]   | 0;
        pad.volume        = padObj["volume"]        | 100;
        pad.pan           = padObj["pan"]           | 0;
        pad.playbackMode  = padObj["playbackMode"]  | (uint8_t)0;
        pad.cycle         = padObj["cycle"]         | false;
        pad.pitch         = padObj["pitch"]         | 0;

        pad.RemoveAllSamples();

        JsonVariant samplesVar = padObj["samples"];
        if (samplesVar.is<JsonArray>()) {
            for (JsonVariant sv : samplesVar.as<JsonArray>()) {
                std::string sampleName;
                uint32_t startF = 0, endF = 0;

                if (sv.is<const char*>()) {
                    sampleName = sv.as<const char*>();
                } else if (sv.is<JsonObject>()) {
                    sampleName = sv["sample"].as<const char*>();
                    startF     = sv["start"] | (uint32_t)0;
                    endF       = sv["end"]   | (uint32_t)0;
                }

                if (!sampleName.empty()) {
                    pad.AddSample(sampleName, false);
                    if (!pad.samples.empty()) {
                        pad.samples.back().start = startF;
                        pad.samples.back().end   = endF;
                    }
                }
            }
        }

        kit.total_samples += pad.sampleCount;
        padIdx++;
    }

    kit.parsed = true;
    return true;
}

bool kit_serialize(const KitInfo& kit, JsonDocument& doc)
{
    doc["name"]        = kit.name;
    doc["author"]      = kit.author;
    doc["description"] = kit.description;
    doc["version"]     = kit.version;
    doc["created_at"]  = kit.created_at;
    doc["type"]        = static_cast<int>(kit.type);

    JsonArray padsArr = doc["pads"].to<JsonArray>();

    for (int i = 0; i < KIT_PADS; i++) {
        const Pad& pad = kit.pads[i];
        JsonObject padObj = padsArr.add<JsonObject>();

        padObj["note"]          = pad.note;
        padObj["color"]         = pad.color;
        padObj["color_pressed"] = pad.color_pressed;
        padObj["choke_group"]   = pad.choke_group;
        padObj["volume"]        = pad.volume;
        padObj["pan"]           = pad.pan;
        padObj["playbackMode"]  = pad.playbackMode;
        padObj["cycle"]         = pad.cycle;

        JsonArray samplesArr = padObj["samples"].to<JsonArray>();
        for (uint8_t s = 0; s < pad.sampleCount && s < (uint8_t)pad.samples.size(); s++) {
            JsonObject sampleObj = samplesArr.add<JsonObject>();
            sampleObj["sample"] = pad.samples[s].sample;
            sampleObj["start"]  = pad.samples[s].start;
            sampleObj["end"]    = pad.samples[s].end;
        }
    }

    return true;
}

} // namespace crosspad

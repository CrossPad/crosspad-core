// SPDX-License-Identifier: MIT
#include "KitJsonParser.hpp"

#include <cstring>

namespace crosspad {

namespace {

uint8_t clampU8(long v, long lo, long hi) {
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return static_cast<uint8_t>(v);
}

} // namespace

bool kit_parse_instrument(KitInfo& kit, JsonDocument& doc)
{
    kit.engine     = KitEngine::Sampler;
    kit.instrument = InstrumentSpec();

    const char* engine = doc["engine"] | "";
    if (std::strcmp(engine, "pitched") != 0) return true;   // sampler, or unknown
    kit.engine = KitEngine::Pitched;

    JsonObject inst = doc["instrument"].as<JsonObject>();
    if (inst.isNull()) return true;

    InstrumentSpec& spec = kit.instrument;

    JsonArray zones = inst["zones"].as<JsonArray>();
    if (!zones.isNull()) {
        for (JsonObject z : zones) {
            if (spec.zones.size() >= kInstrumentMaxZones) break;

            const char* sample = z["sample"] | "";
            if (sample[0] == 0) continue;                   // a zone without a file is nothing

            InstrumentZoneSpec zone;
            zone.sample = sample;

            JsonVariant root = z["root"];
            // A number is the root note (57 or 57.5); "auto", anything else or
            // absent leaves -1 for the load-time pitch detector.
            if (!root.is<const char*>() && root.is<float>())
                zone.root = root.as<float>();

            zone.loopStart = z["loop_start"] | (uint32_t)0;
            zone.loopEnd   = z["loop_end"]   | (uint32_t)0;

            spec.zones.push_back(zone);
        }
    }

    JsonArray padNotes = inst["pad_notes"].as<JsonArray>();
    if (!padNotes.isNull() && padNotes.size() == (size_t)KIT_PADS) {
        uint8_t staged[KIT_PADS];
        bool    ok = true;
        int     i  = 0;
        for (JsonVariant v : padNotes) {
            if (!v.is<int>()) { ok = false; break; }
            int note = v.as<int>();
            if (note < 0 || note > 127) { ok = false; break; }
            staged[i++] = static_cast<uint8_t>(note);
        }
        if (ok) {
            std::memcpy(spec.padNotes, staged, sizeof(staged));
            spec.padNotesSet = true;
        }
    }

    spec.baseNote  = clampU8(inst["base_note"] | 60, 0, 127);
    spec.releaseMs = static_cast<uint16_t>(inst["release_ms"] | 150);
    spec.attackMs  = static_cast<uint16_t>(inst["attack_ms"]  | 2);
    spec.maxVoices = clampU8(inst["max_voices"] | 24, 1, 32);

    kit.total_samples = static_cast<int>(spec.zones.size());
    return true;
}

bool kit_parse_metadata(KitInfo& kit, JsonDocument& doc)
{
    kit.name        = doc["name"]        | "Kit";
    kit.author      = doc["author"]      | "";
    kit.description = doc["description"] | "";
    kit.version     = doc["version"]     | "";
    kit.created_at  = doc["created_at"]  | "";
    kit.category    = doc["category"]    | "";
    kit.bpm         = doc["bpm"]         | (uint16_t)0;

    if (doc["type"].is<int>())
        kit.type = static_cast<KitInfo::Type>(doc["type"].as<int>());

    kit_parse_instrument(kit, doc);

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

    // A pitched kit's samples are its zones; the pads carry no files.
    if (kit.engine == KitEngine::Pitched)
        kit.total_samples = static_cast<int>(kit.instrument.zones.size());

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
    if (!kit.category.empty()) doc["category"] = kit.category;
    if (kit.bpm) doc["bpm"] = kit.bpm;   // absent rather than 0 when unset
    doc["type"]        = static_cast<int>(kit.type);

    if (kit.engine == KitEngine::Pitched) {
        doc["engine"] = "pitched";

        const InstrumentSpec& spec = kit.instrument;
        JsonObject inst = doc["instrument"].to<JsonObject>();

        JsonArray zones = inst["zones"].to<JsonArray>();
        for (const InstrumentZoneSpec& z : spec.zones) {
            JsonObject zo = zones.add<JsonObject>();
            zo["sample"] = z.sample;
            if (z.root < 0.0f) zo["root"] = "auto";
            else               zo["root"] = z.root;
            zo["loop_start"] = z.loopStart;
            zo["loop_end"]   = z.loopEnd;
        }

        if (spec.padNotesSet) {
            JsonArray notes = inst["pad_notes"].to<JsonArray>();
            for (int i = 0; i < KIT_PADS; i++) notes.add(spec.padNotes[i]);
        }

        inst["base_note"]  = spec.baseNote;
        inst["release_ms"] = spec.releaseMs;
        inst["attack_ms"]  = spec.attackMs;
        inst["max_voices"] = spec.maxVoices;
    }

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

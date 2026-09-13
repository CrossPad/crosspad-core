// SPDX-License-Identifier: MIT
#include "crosspad/sequencer/SequenceStore.hpp"

#include <ArduinoJson.h>

#include <cstdio>
#include <fstream>

namespace crosspad {

bool SequenceStore::save(const PatternSequencer& seq, const std::string& path)
{
    /* Write beside the real file and rename over it. A card pulled or a board
     * reset halfway through then costs the *new* take, never the one already
     * on the card -- an SD write is not atomic and this file is the only copy
     * of somebody's pattern.
     *
     * The caller is expected not to save while recording; a hit landing during
     * the build below can still reach the file as a stale velocity, which the
     * next save corrects. That is deliberate: locking the pattern for the
     * length of a card write would stall the beat, and a wrong velocity in one
     * event is a far smaller price. */
    const std::string tmp = path + ".tmp";
    std::ofstream file(tmp);
    if (!file.is_open()) return false;

    JsonDocument doc;
    doc["version"] = kVersion;
    doc["ppq"]     = (int)PatternSequencer::kPpq;
    doc["bpm"]     = seq.getClickBpm();
    doc["active"]  = seq.activeScene();

    /* As many scenes as the kit has, not as many as the engine could hold: the
     * file is what says how many a kit came with. */
    JsonArray scenes = doc["scenes"].to<JsonArray>();
    for (uint8_t s = 0; s < seq.sceneCount(); ++s) {
        JsonObject o = scenes.add<JsonObject>();
        o["bars"] = seq.sceneBars(s);
        JsonArray evs = o["events"].to<JsonArray>();
        for (uint16_t i = 0; i < seq.eventCount(s); ++i) {
            uint32_t ticks = 0; uint8_t pad = 0, vel = 0;
            if (!seq.eventAtTicks(s, i, ticks, pad, vel)) continue;
            /* A triple rather than named fields: a full scene is 192 of these
             * and the card is slow enough that the shape matters. */
            JsonArray e = evs.add<JsonArray>();
            e.add(ticks);
            e.add(pad);
            e.add(vel);
        }
    }

    serializeJson(doc, file);
    file.close();
    if (!file) { std::remove(tmp.c_str()); return false; }

    std::remove(path.c_str());          // rename() will not replace on FatFs
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool SequenceStore::load(PatternSequencer& seq, const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open()) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    if (err) return false;

    if (doc["version"].as<int>() > kVersion) return false;

    /* Tempo first: every tick-to-sample conversion below reads it, and so does
     * the trim inside setSceneBars. */
    const float bpm = doc["bpm"] | 0.0f;
    if (bpm > 0.0f) seq.setClickBpm(bpm);

    const int ppq = doc["ppq"] | (int)PatternSequencer::kPpq;

    seq.clearAll(0, 127);

    JsonArrayConst scenes = doc["scenes"].as<JsonArrayConst>();
    /* The count first: setSceneBars() and addEventTicks() both refuse a scene
     * the kit does not have. A file from before scenes were countable holds
     * four, which is exactly what it will say here. */
    size_t sceneN = scenes.size();
    if (sceneN == 0) sceneN = 1;
    if (sceneN > PatternSequencer::kNumSequences) sceneN = PatternSequencer::kNumSequences;
    seq.setSceneCount((uint8_t)sceneN);

    uint8_t s = 0;
    for (JsonObjectConst o : scenes) {
        if (s >= seq.sceneCount()) break;
        /* Bars before events: an event is refused if it falls past the scene's
         * end, and the end is not known until the length is. */
        const uint8_t bars = o["bars"] | 4;
        seq.setSceneBars(s, bars);

        for (JsonArrayConst e : o["events"].as<JsonArrayConst>()) {
            if (e.size() < 3) continue;
            uint32_t ticks = e[0].as<uint32_t>();
            /* A file written with a different resolution still lands on the
             * right beat; only its sub-tick detail is lost. */
            if (ppq > 0 && ppq != (int)PatternSequencer::kPpq) {
                ticks = (uint32_t)(((uint64_t)ticks * PatternSequencer::kPpq) / (uint32_t)ppq);
            }
            seq.addEventTicks(s, ticks, e[1].as<uint8_t>(), e[2].as<uint8_t>());
        }
        ++s;
    }

    seq.selectScene(doc["active"] | 0);
    /* Freshly loaded is exactly what the card holds. */
    seq.clearDirty();
    return true;
}

} // namespace crosspad

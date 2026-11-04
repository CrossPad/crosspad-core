// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "../src/kit/KitJsonParser.hpp"

#include <string>

using crosspad::KitEngine;
using crosspad::KitInfo;

namespace {

// The example from docs/superpowers/specs/2026-09-03-pitched-instrument-design.md.
const char* kPitchedKitJson = R"({
  "name": "HIL SINE PITCHED", "author": "hil", "engine": "pitched", "type": 0,
  "instrument": {
    "zones": [
      { "sample": "a3.wav", "root": 57 },
      { "sample": "a4.wav", "root": "auto", "loop_start": 0, "loop_end": 0 }
    ],
    "pad_notes": [60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75],
    "base_note": 60,
    "release_ms": 150, "attack_ms": 2, "max_voices": 24
  },
  "pads": [
    { "note": 0,  "color": 100, "playbackMode": 1, "volume": 100, "pan": 0 },
    { "note": 1,  "color": 100, "playbackMode": 1, "volume": 100, "pan": 0 },
    { "note": 2,  "color": 100, "playbackMode": 1, "volume": 100, "pan": 0 },
    { "note": 3,  "color": 100, "playbackMode": 1, "volume": 100, "pan": 0 },
    { "note": 4,  "color": 100, "playbackMode": 1, "volume": 100, "pan": 0 },
    { "note": 5,  "color": 100, "playbackMode": 1, "volume": 100, "pan": 0 },
    { "note": 6,  "color": 100, "playbackMode": 1, "volume": 100, "pan": 0 },
    { "note": 7,  "color": 100, "playbackMode": 1, "volume": 100, "pan": 0 },
    { "note": 8,  "color": 100, "playbackMode": 1, "volume": 100, "pan": 0 },
    { "note": 9,  "color": 100, "playbackMode": 1, "volume": 100, "pan": 0 },
    { "note": 10, "color": 100, "playbackMode": 1, "volume": 100, "pan": 0 },
    { "note": 11, "color": 100, "playbackMode": 1, "volume": 100, "pan": 0 },
    { "note": 12, "color": 100, "playbackMode": 1, "volume": 100, "pan": 0 },
    { "note": 13, "color": 100, "playbackMode": 1, "volume": 100, "pan": 0 },
    { "note": 14, "color": 100, "playbackMode": 1, "volume": 100, "pan": 0 },
    { "note": 15, "color": 100, "playbackMode": 1, "volume": 100, "pan": 0 }
  ]
})";

KitInfo parse(const char* json, bool pads = true) {
    JsonDocument doc;
    REQUIRE(deserializeJson(doc, json) == DeserializationError::Ok);
    KitInfo kit;
    REQUIRE(crosspad::kit_parse_metadata(kit, doc));
    if (pads) REQUIRE(crosspad::kit_parse_pads(kit, doc));
    return kit;
}

std::string serialize(const KitInfo& kit) {
    JsonDocument doc;
    REQUIRE(crosspad::kit_serialize(kit, doc));
    std::string out;
    serializeJson(doc, out);
    return out;
}

} // namespace

TEST_CASE("kit json: pitched instrument block is parsed", "[kit][instrument]") {
    KitInfo kit = parse(kPitchedKitJson);

    REQUIRE(kit.engine == KitEngine::Pitched);
    REQUIRE(kit.name == "HIL SINE PITCHED");
    REQUIRE(kit.author == "hil");

    const auto& spec = kit.instrument;
    REQUIRE(spec.zones.size() == 2);
    REQUIRE(spec.zones[0].sample == "a3.wav");
    REQUIRE(spec.zones[0].root == Catch::Approx(57.0f));
    REQUIRE(spec.zones[1].sample == "a4.wav");
    REQUIRE(spec.zones[1].root < 0.0f);          // "auto"
    REQUIRE(spec.zones[1].loopStart == 0);
    REQUIRE(spec.zones[1].loopEnd == 0);

    REQUIRE(spec.padNotesSet);
    for (uint8_t p = 0; p < KIT_PADS; p++)
        REQUIRE(spec.noteForPad(p) == 60 + p);

    REQUIRE(spec.baseNote == 60);
    REQUIRE(spec.releaseMs == 150);
    REQUIRE(spec.attackMs == 2);
    REQUIRE(spec.maxVoices == 24);

    // Zones are the kit's samples; the pads carry no files.
    REQUIRE(kit.total_samples == 2);
    REQUIRE(kit.pads[0].playbackMode == 1);
    REQUIRE(kit.pads[15].note == 15);
    REQUIRE(kit.parsed);
}

TEST_CASE("kit json: metadata-only parse sees the engine too", "[kit][instrument]") {
    KitInfo kit = parse(kPitchedKitJson, /*pads=*/false);
    REQUIRE(kit.engine == KitEngine::Pitched);
    REQUIRE(kit.instrument.zones.size() == 2);
    REQUIRE(kit.total_samples == 2);
}

TEST_CASE("kit json: absent engine is the sampler", "[kit][instrument]") {
    KitInfo kit = parse(R"({"name":"Plain","pads":[{"note":0,"samples":["1.wav"]}]})");
    REQUIRE(kit.engine == KitEngine::Sampler);
    REQUIRE(kit.instrument.zones.empty());
    REQUIRE_FALSE(kit.instrument.padNotesSet);
    REQUIRE(kit.total_samples == 1);

    KitInfo named = parse(R"({"name":"Plain","engine":"sampler","pads":[]})");
    REQUIRE(named.engine == KitEngine::Sampler);

    KitInfo junk = parse(R"({"name":"Plain","engine":"granular","pads":[]})");
    REQUIRE(junk.engine == KitEngine::Sampler);
}

TEST_CASE("kit json: noteForPad falls back to base_note", "[kit][instrument]") {
    KitInfo kit = parse(R"({"engine":"pitched","instrument":{
        "zones":[{"sample":"a.wav"}], "base_note": 48 }, "pads":[]})");

    REQUIRE_FALSE(kit.instrument.padNotesSet);
    REQUIRE(kit.instrument.baseNote == 48);
    REQUIRE(kit.instrument.noteForPad(0) == 48);
    REQUIRE(kit.instrument.noteForPad(15) == 63);
    REQUIRE(kit.instrument.zones[0].root < 0.0f);   // "root" absent → auto

    // Out-of-range pad indices never read past the array.
    REQUIRE(kit.instrument.noteForPad(KIT_PADS) == 48);

    crosspad::InstrumentSpec high;
    high.baseNote = 120;
    REQUIRE(high.noteForPad(15) == 127);            // clamped, not wrapped
}

TEST_CASE("kit json: pitched kit survives a serialize/parse round trip", "[kit][instrument]") {
    KitInfo kit = parse(kPitchedKitJson);
    std::string once = serialize(kit);

    KitInfo back = parse(once.c_str());
    REQUIRE(back.engine == KitEngine::Pitched);
    REQUIRE(back.instrument.zones.size() == kit.instrument.zones.size());
    for (size_t z = 0; z < back.instrument.zones.size(); z++) {
        REQUIRE(back.instrument.zones[z].sample == kit.instrument.zones[z].sample);
        REQUIRE(back.instrument.zones[z].root == Catch::Approx(kit.instrument.zones[z].root));
        REQUIRE(back.instrument.zones[z].loopStart == kit.instrument.zones[z].loopStart);
        REQUIRE(back.instrument.zones[z].loopEnd == kit.instrument.zones[z].loopEnd);
    }
    REQUIRE(back.instrument.padNotesSet);
    for (uint8_t p = 0; p < KIT_PADS; p++)
        REQUIRE(back.instrument.noteForPad(p) == kit.instrument.noteForPad(p));
    REQUIRE(back.instrument.baseNote == kit.instrument.baseNote);
    REQUIRE(back.instrument.releaseMs == kit.instrument.releaseMs);
    REQUIRE(back.instrument.attackMs == kit.instrument.attackMs);
    REQUIRE(back.instrument.maxVoices == kit.instrument.maxVoices);
    REQUIRE(back.total_samples == kit.total_samples);

    REQUIRE(serialize(back) == once);

    // "auto" stays "auto" on the way out — a detected root is not baked in.
    REQUIRE(once.find("\"root\":\"auto\"") != std::string::npos);
}

TEST_CASE("kit json: a sampler kit serializes without engine or instrument",
          "[kit][instrument]") {
    KitInfo kit = parse(R"({"name":"Plain","pads":[{"note":0,"samples":["1.wav"]}]})");
    std::string out = serialize(kit);

    REQUIRE(out.find("engine") == std::string::npos);
    REQUIRE(out.find("instrument") == std::string::npos);
    REQUIRE(out.find("\"pads\"") != std::string::npos);
}

TEST_CASE("kit json: zones past the bank limit are dropped", "[kit][instrument]") {
    std::string zones;
    for (int i = 1; i <= crosspad::kInstrumentMaxZones + 1; ++i) {
        if (i > 1) zones += ",";
        zones += "{\"sample\":\"" + std::to_string(i) + ".wav\"}";
    }
    const std::string json = R"({"engine":"pitched","instrument":{"zones":[)" + zones + R"(]},"pads":[]})";
    KitInfo kit = parse(json.c_str());

    REQUIRE(kit.instrument.zones.size() == crosspad::kInstrumentMaxZones);
    REQUIRE(kit.instrument.zones.back().sample == std::to_string(crosspad::kInstrumentMaxZones) + ".wav");
    REQUIRE(kit.total_samples == crosspad::kInstrumentMaxZones);
}

TEST_CASE("kit json: a zone without a sample is skipped", "[kit][instrument]") {
    KitInfo kit = parse(R"({"engine":"pitched","instrument":{"zones":[
        {"root":57},{"sample":"good.wav","root":60,"loop_start":100,"loop_end":900}
        ]},"pads":[]})");

    REQUIRE(kit.instrument.zones.size() == 1);
    REQUIRE(kit.instrument.zones[0].sample == "good.wav");
    REQUIRE(kit.instrument.zones[0].root == Catch::Approx(60.0f));
    REQUIRE(kit.instrument.zones[0].loopStart == 100);
    REQUIRE(kit.instrument.zones[0].loopEnd == 900);
}

TEST_CASE("kit json: a short pad_notes array is ignored", "[kit][instrument]") {
    KitInfo kit = parse(R"({"engine":"pitched","instrument":{
        "zones":[{"sample":"a.wav"}],
        "pad_notes":[60,61,62,63,64,65,66,67,68,69,70,71,72,73,74],
        "base_note":36},"pads":[]})");

    REQUIRE_FALSE(kit.instrument.padNotesSet);
    REQUIRE(kit.instrument.noteForPad(0) == 36);

    // So is one with an out-of-range entry.
    KitInfo bad = parse(R"({"engine":"pitched","instrument":{
        "zones":[{"sample":"a.wav"}],
        "pad_notes":[60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,200]},
        "pads":[]})");
    REQUIRE_FALSE(bad.instrument.padNotesSet);
}

TEST_CASE("kit json: fractional roots and max_voices clamping", "[kit][instrument]") {
    KitInfo kit = parse(R"({"engine":"pitched","instrument":{
        "zones":[{"sample":"a.wav","root":57.5}], "max_voices": 99},"pads":[]})");
    REQUIRE(kit.instrument.zones[0].root == Catch::Approx(57.5f));
    REQUIRE(kit.instrument.maxVoices == 32);

    KitInfo zero = parse(R"({"engine":"pitched","instrument":{
        "zones":[{"sample":"a.wav"}], "max_voices": 0},"pads":[]})");
    REQUIRE(zero.instrument.maxVoices == 1);
}

TEST_CASE("kit json: getDetails names the engine", "[kit][instrument]") {
    KitInfo sampler;
    REQUIRE(sampler.getDetails().find("sampler") != std::string::npos);

    KitInfo pitched = parse(kPitchedKitJson);
    REQUIRE(pitched.getDetails().find("pitched") != std::string::npos);
    REQUIRE(pitched.getDetails().find("HIL SINE PITCHED") == 0);
}

TEST_CASE("kit json: category files a kit as the user's, the Slicer's without it too", "[kit]") {
    using namespace crosspad;
    JsonDocument doc;
    deserializeJson(doc, R"({"name":"Mine","author":"Someone","category":"user"})");
    KitInfo a;
    kit_parse_metadata(a, doc);
    CHECK(a.category == "user");
    CHECK(a.isUser());

    JsonDocument old;
    deserializeJson(old, R"({"name":"REC_012","author":"CrossPad Slicer"})");
    KitInfo b;
    kit_parse_metadata(b, old);
    CHECK(b.category.empty());
    CHECK(b.isUser());                          // made before the Slicer wrote a category

    JsonDocument stock;
    deserializeJson(stock, R"({"name":"Black Tesla","author":"CrossPad"})");
    KitInfo c;
    kit_parse_metadata(c, stock);
    CHECK_FALSE(c.isUser());

    JsonDocument out;
    kit_serialize(a, out);
    CHECK(std::string(out["category"] | "") == "user");
    JsonDocument out2;
    kit_serialize(c, out2);
    CHECK_FALSE(out2["category"].is<const char*>());   // absent, not "", for a stock kit
    CHECK_FALSE(out2["own"].is<bool>());
}

TEST_CASE("kit json: a drum kit made on the board is drums and the user's", "[kit]") {
    using namespace crosspad;
    JsonDocument doc;
    deserializeJson(doc, R"({"name":"NEW DRUMS","category":"drums","own":true})");
    KitInfo k;
    kit_parse_metadata(k, doc);
    CHECK(k.isDrums());
    CHECK(k.isUser());
    CHECK(k.voiceCap() == KitInfo::kDrumVoices);

    JsonDocument out;
    kit_serialize(k, out);
    CHECK((out["own"] | false));
    CHECK(std::string(out["category"] | "") == "drums");
}

TEST_CASE("kit json: a pad's pitch and filters survive a round trip, and 0 is not written", "[kit]") {
    KitInfo kit;
    kit.pads[3].pitch = -5;
    kit.pads[3].hp = 200;
    kit.pads[3].lp = 8000;
    kit.pads[3].reverse = true;
    const std::string json = serialize(kit);
    CHECK(json.find("\"pitch\":-5") != std::string::npos);
    CHECK(json.find("\"pitch\":0") == std::string::npos);
    const KitInfo back = parse(json.c_str());
    CHECK(back.pads[3].pitch == -5);
    CHECK(back.pads[3].hp == 200);
    CHECK(back.pads[3].lp == 8000);
    CHECK(back.pads[3].reverse);
    CHECK_FALSE(back.pads[4].reverse);
    CHECK(json.find("\"reverse\":false") == std::string::npos);
    CHECK(back.pads[4].pitch == 0);
    CHECK(back.pads[4].hp == 0);
    CHECK(json.find("\"hp\":0") == std::string::npos);
}

TEST_CASE("kit json: a zone keeps its start and loop crossfade", "[kit][instrument]") {
    KitInfo kit = parse(R"({"engine":"pitched","instrument":{"zones":[
        {"sample":"a.wav","root":60,"start":40,"loop_start":100,"loop_end":900,"loop_xfade":80},
        {"sample":"b.wav","root":62,"loop_start":100,"loop_end":900},
        {"sample":"c.wav","root":64,"loop_start":100,"loop_end":0}
        ]},"pads":[]})");

    REQUIRE(kit.instrument.zones[0].start == 40);
    REQUIRE(kit.instrument.zones[0].loopXfade == 80);
    // Written before "start" existed: a looped note began at its loop start,
    // and still does; a one-shot began at 0.
    REQUIRE(kit.instrument.zones[1].start == 100);
    REQUIRE(kit.instrument.zones[1].loopXfade == 0);
    REQUIRE(kit.instrument.zones[2].start == 0);

    KitInfo back = parse(serialize(kit).c_str(), false);
    REQUIRE(back.instrument.zones[0].start == 40);
    REQUIRE(back.instrument.zones[0].loopXfade == 80);
    REQUIRE(back.instrument.zones[1].start == 100);
    REQUIRE(back.instrument.zones[2].start == 0);
}

TEST_CASE("kit json: crosspad.app's \"mode\" maps onto playbackMode and cycle", "[kit]") {
    const KitInfo kit = parse(R"({ "name": "APP", "pads": [
        { "mode": "tap" }, { "mode": "hold" }, { "mode": "toggle" }, { "mode": "cycle" },
        { "mode": "cycle", "cycle": false }, { "mode": "hold", "playbackMode": 2 }
    ] })");
    CHECK(kit.pads[0].playbackMode == 0);
    CHECK_FALSE(kit.pads[0].cycle);
    CHECK(kit.pads[1].playbackMode == 1);
    CHECK(kit.pads[2].playbackMode == 2);
    CHECK(kit.pads[3].playbackMode == 0);
    CHECK(kit.pads[3].cycle);
    CHECK_FALSE(kit.pads[4].cycle);             // an explicit key beats "mode"
    CHECK(kit.pads[5].playbackMode == 2);
}

TEST_CASE("kit json: a saved crosspad.app kit is written in the firmware's keys", "[kit]") {
    const KitInfo kit = parse(R"({ "name": "APP", "pads": [ { "mode": "hold" }, { "mode": "cycle" } ] })");
    const std::string json = serialize(kit);
    CHECK(json.find("\"mode\"") == std::string::npos);
    const KitInfo back = parse(json.c_str());
    CHECK(back.pads[0].playbackMode == 1);
    CHECK(back.pads[1].cycle);
}

TEST_CASE("kit json: a Slice kit is one file in regions, and comes back as it went", "[kitjson][slice]") {
    const char* json = R"({
      "name": "AMEN", "author": "CrossPad Slicer", "category": "slice", "engine": "slice", "pitch": -2,
      "pads": [
        { "samples": [ { "sample": "loop.wav", "start": 0, "end": 5000 } ], "attack": 4 },
        { "samples": [ { "sample": "loop.wav", "start": 5000, "end": 9000 } ] }
      ]
    })";
    KitInfo kit = parse(json);
    CHECK(kit.engine == KitEngine::Slice);
    CHECK(kit.playsOnSampler());
    CHECK(kit.pitch == -2);
    CHECK(std::string(kit.engineName()) == "slice");
    REQUIRE(kit.pads[1].samples.size() == 1);
    CHECK(kit.pads[1].samples[0].start == 5000);
    CHECK(kit.pads[1].samples[0].end == 9000);
    CHECK(kit.pads[0].attack == 4);
    CHECK(kit.pads[1].attack == 0);

    JsonDocument out;
    REQUIRE(crosspad::kit_serialize(kit, out));
    std::string text;
    serializeJson(out, text);
    KitInfo back = parse(text.c_str());
    CHECK(back.engine == KitEngine::Slice);
    CHECK(back.pitch == -2);
    CHECK(back.pads[0].attack == 4);
    CHECK(back.pads[1].samples[0].start == 5000);
}

TEST_CASE("kit json: drums are told by their category, any case", "[kitjson]") {
    KitInfo kit;
    kit.category = "DRUMS";
    CHECK(kit.isDrums());
    CHECK(kit.voiceCap() == KitInfo::kDrumVoices);
    kit.category = "drum";
    CHECK(kit.isDrums());
    kit.category = "drumsets";
    CHECK_FALSE(kit.isDrums());
    CHECK(kit.voiceCap() == 0);
}

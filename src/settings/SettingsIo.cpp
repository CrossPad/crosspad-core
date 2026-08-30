// SPDX-License-Identifier: MIT
#include "crosspad/settings/SettingsIo.hpp"

#include <ArduinoJson.h>
#include <cstdio>
#include <cstring>
#include <memory>

namespace crosspad {
namespace {

constexpr const char* kKindAll  = "crosspad-settings";
constexpr const char* kKindPads = "crosspad-pads";

// Static strings — SettingsIoResult::error is a borrowed pointer the UI keeps
// after the call returns.
constexpr const char* kErrOpen      = "cannot open file";
constexpr const char* kErrWrite     = "write failed";
constexpr const char* kErrParse     = "not valid JSON";
constexpr const char* kErrKind      = "not a CrossPad settings file";
constexpr const char* kErrVersion   = "file is newer than this firmware";
constexpr const char* kErrMalformed = "malformed settings file";

// ArduinoJson only ships Arduino Stream and std::istream adapters. stdio is
// the one file API shared by the device's SD VFS and the host, and it keeps
// <fstream>'s locale machinery off the LVGL task's stack.
struct FileReader {
    FILE* f;
    int read() { return fgetc(f); }
    size_t readBytes(char* buf, size_t n) { return fread(buf, 1, n, f); }
};

struct FileWriter {
    FILE* f;
    size_t write(uint8_t c) { return fputc((int)c, f) == EOF ? 0u : 1u; }
    size_t write(const uint8_t* s, size_t n) { return fwrite(s, 1, n, f); }
};

SettingsIoResult fail(const char* error, uint16_t version = 0) {
    SettingsIoResult r;
    r.error = error;
    r.version = version;
    return r;
}

SettingsIoResult okResult(uint16_t version) {
    SettingsIoResult r;
    r.ok = true;
    r.version = version;
    return r;
}

/// Same clamped read as Stm32Settings uses, for the scalars that live directly
/// on CrosspadSettings.
long long jsonInt(JsonVariantConst v, long long cur, long long lo, long long hi) {
    if (!v.is<long long>()) return cur;
    long long n = v.as<long long>();
    return n < lo ? lo : (n > hi ? hi : n);
}

SettingsIoResult writeDoc(const JsonDocument& doc, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return fail(kErrOpen);

    FileWriter w{f};
    size_t written = serializeJson(doc, w);
    bool ioError = ferror(f) != 0;
    if (fclose(f) != 0 || ioError || written == 0) return fail(kErrWrite);

    return okResult(kSettingsFileVersion);
}

/// Read the envelope and return the payload. Kind and version are checked
/// first, so a foreign or too-new file costs nothing but a parse.
SettingsIoResult readEnvelope(const char* path, const char* kind, const char* payloadKey,
                              JsonDocument& doc, JsonObject& payload) {
    FILE* f = fopen(path, "r");
    if (!f) return fail(kErrOpen);

    FileReader r{f};
    DeserializationError err = deserializeJson(doc, r);
    fclose(f);
    if (err) return fail(kErrParse);

    if (strcmp(doc["kind"] | "", kind) != 0) return fail(kErrKind);
    if (!doc["version"].is<long long>()) return fail(kErrMalformed);

    long long version = doc["version"].as<long long>();
    if (version < 1 || version > 0xFFFF) return fail(kErrMalformed);
    // Older files are fine (missing keys keep their value), but a newer schema
    // may reinterpret keys we already know.
    if (version > kSettingsFileVersion) return fail(kErrVersion, (uint16_t)version);

    if (!doc[payloadKey].is<JsonObject>()) return fail(kErrMalformed, (uint16_t)version);
    payload = doc[payloadKey].as<JsonObject>();
    return okResult((uint16_t)version);
}

} // namespace

SettingsIoResult settingsExportAll(const CrosspadSettings& s, const char* path) {
    JsonDocument doc;   // ArduinoJson v7 documents grow on demand — no capacity to size
    doc["kind"] = kKindAll;
    doc["version"] = kSettingsFileVersion;
    JsonObject out = doc["settings"].to<JsonObject>();

    JsonObject keypad = out["keypad"].to<JsonObject>();
    s.keypad.toJson(keypad);
    JsonObject stmRouting = out["stmRouting"].to<JsonObject>();
    s.stmRouting.toJson(stmRouting);
    JsonObject esp32Routing = out["esp32Routing"].to<JsonObject>();
    s.esp32Routing.toJson(esp32Routing);
    JsonObject wireless = out["wireless"].to<JsonObject>();
    s.wireless.toJson(wireless);

    JsonArray audio = out["audio"].to<JsonArray>();
    for (int i = 0; i < CROSSPAD_MAX_PADS; i++) {
        JsonObject a = audio.add<JsonObject>();
        s.audio[i].toJson(a);
    }
    JsonObject previewAudio = out["previewAudio"].to<JsonObject>();
    s.previewAudio.toJson(previewAudio);
    JsonObject masterFX = out["masterFX"].to<JsonObject>();
    s.masterFX.toJson(masterFX);

    // AudioEngineSettings has no toJson: two restart-time fields, validated on
    // import against its own isValid* helpers.
    JsonObject audioEngine = out["audioEngine"].to<JsonObject>();
    audioEngine["sampleRate"] = s.audioEngine.sampleRate;
    audioEngine["frameCount"] = s.audioEngine.frameCount;

    JsonObject vibration = out["vibration"].to<JsonObject>();
    s.vibration.toJson(vibration);
    JsonObject stm = out["stm"].to<JsonObject>();
    s.stm.toJson(stm);

    out["LCDbrightness"] = s.LCDbrightness;
    out["themeColorIndex"] = s.themeColorIndex;
    out["perfStatsFlags"] = s.perfStatsFlags;
    out["RGBbrightness"] = s.RGBbrightness;
    out["quickSettingsEnabled"] = s.quickSettingsEnabled;
    out["AudioEngineEnabled"] = s.AudioEngineEnabled;
    out["usbAudioEnabled"] = s.usbAudioEnabled;
    out["usbAudioPreset"] = s.usbAudioPreset;
    out["Kit"] = s.Kit;

    return writeDoc(doc, path);
}

SettingsIoResult settingsImportAll(CrosspadSettings& s, const char* path) {
    JsonDocument doc;
    JsonObject in;
    SettingsIoResult res = readEnvelope(path, kKindAll, "settings", doc, in);
    if (!res.ok) return res;

    // Parse into a copy: the audio and STM tasks read the live settings, and a
    // half-applied file would reach the hardware. On the heap — this is a few
    // kB on the LVGL task's stack otherwise.
    std::unique_ptr<CrosspadSettings> tmp(new CrosspadSettings(s));

    if (in["keypad"].is<JsonObject>())       tmp->keypad.fromJson(in["keypad"]);
    if (in["stmRouting"].is<JsonObject>())   tmp->stmRouting.fromJson(in["stmRouting"]);
    if (in["esp32Routing"].is<JsonObject>()) tmp->esp32Routing.fromJson(in["esp32Routing"]);
    if (in["wireless"].is<JsonObject>())     tmp->wireless.fromJson(in["wireless"]);

    if (in["audio"].is<JsonArray>()) {
        JsonArray audio = in["audio"].as<JsonArray>();
        // A shorter array leaves the remaining pads as they were.
        for (int i = 0; i < CROSSPAD_MAX_PADS && i < (int)audio.size(); i++) {
            if (audio[i].is<JsonObject>()) tmp->audio[i].fromJson(audio[i]);
        }
    }
    if (in["previewAudio"].is<JsonObject>()) tmp->previewAudio.fromJson(in["previewAudio"]);
    if (in["masterFX"].is<JsonObject>())     tmp->masterFX.fromJson(in["masterFX"]);

    if (in["audioEngine"].is<JsonObject>()) {
        JsonObject engine = in["audioEngine"].as<JsonObject>();
        uint32_t sampleRate = engine["sampleRate"] | tmp->audioEngine.sampleRate;
        uint32_t frameCount = engine["frameCount"] | tmp->audioEngine.frameCount;
        // Not clampable — only the enumerated values boot the I2S clock.
        if (AudioEngineSettings::isValidSampleRate(sampleRate)) tmp->audioEngine.sampleRate = sampleRate;
        if (AudioEngineSettings::isValidFrameCount(frameCount)) tmp->audioEngine.frameCount = frameCount;
    }

    if (in["vibration"].is<JsonObject>()) tmp->vibration.fromJson(in["vibration"]);
    if (in["stm"].is<JsonObject>())       tmp->stm.fromJson(in["stm"]);

    tmp->LCDbrightness = (uint8_t)jsonInt(in["LCDbrightness"], tmp->LCDbrightness, 0, 100);
    tmp->themeColorIndex = (uint8_t)jsonInt(in["themeColorIndex"], tmp->themeColorIndex, 0, 255);
    tmp->perfStatsFlags = (uint8_t)jsonInt(in["perfStatsFlags"], tmp->perfStatsFlags, 0, 7);
    tmp->RGBbrightness = (uint8_t)jsonInt(in["RGBbrightness"], tmp->RGBbrightness, 0, 100);
    tmp->quickSettingsEnabled = in["quickSettingsEnabled"] | tmp->quickSettingsEnabled;
    tmp->AudioEngineEnabled = in["AudioEngineEnabled"] | tmp->AudioEngineEnabled;
    tmp->usbAudioEnabled = in["usbAudioEnabled"] | tmp->usbAudioEnabled;
    tmp->usbAudioPreset = in["usbAudioPreset"] | tmp->usbAudioPreset;
    tmp->Kit = (uint8_t)jsonInt(in["Kit"], tmp->Kit, 0, 255);

    s = *tmp;
    return res;
}

SettingsIoResult settingsExportPads(const CrosspadSettings& s, const char* path) {
    JsonDocument doc;
    doc["kind"] = kKindPads;
    doc["version"] = kSettingsFileVersion;
    JsonObject pads = doc["pads"].to<JsonObject>();
    s.stm.padsToJson(pads);

    return writeDoc(doc, path);
}

SettingsIoResult settingsImportPads(CrosspadSettings& s, const char* path) {
    JsonDocument doc;
    JsonObject in;
    SettingsIoResult res = readEnvelope(path, kKindPads, "pads", doc, in);
    if (!res.ok) return res;

    // The STM section only: everything outside pad tuning must survive.
    Stm32Settings tmp = s.stm;
    tmp.padsFromJson(in);
    s.stm = tmp;

    return res;
}

} // namespace crosspad
